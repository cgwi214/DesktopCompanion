// ============================================================================
//  I18n.cpp — 界面多语言实现
//
//  需求依据：SRS V1.1 4.2 TXT-001
//
//  数据来源：resources/i18n/<code>.json，结构是扁平的 key -> 文案映射：
//      {
//        "schemaVersion": 1,
//        "language": "zh-CN",
//        "strings": {
//          "app.name": "桌面宠物助手",
//          "dialog.permission.title": "需要你确认"
//        }
//      }
//
//  为什么用扁平结构而不是嵌套对象：
//    取文案时用点分 key 直接查表，不需要递归下降。
//    嵌套结构在查 "a.b.c" 时要逐层判断类型，代码更绕且更容易出错，
//    而收益只是文件看起来"更整齐" —— 不值得。
// ============================================================================
#include "ui/I18n.h"

#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>

namespace dc {

// ---------------------------------------------------------------------------
// 日志用的安全文件名
//
//  SRS 7.3 要求日志里不出现用户的完整目录结构。
//  这里只取文件名，丢掉的目录信息对排查"哪个语言文件坏了"没有影响。
// ---------------------------------------------------------------------------
static QString redactedFileName(const QString &path)
{
    return QFileInfo(path).fileName();
}

// ---------------------------------------------------------------------------
// 单例
// ---------------------------------------------------------------------------
I18n &I18n::instance()
{
    static I18n s_instance;
    return s_instance;
}

I18n::I18n()
    : QObject(nullptr)   // 单例不挂在其它对象下，避免被提前销毁
{
}

I18n::~I18n() = default;

// ---------------------------------------------------------------------------
// 语言代码
// ---------------------------------------------------------------------------
QString I18n::languageCode() const
{
    return languageToCode(m_language);
}

QString I18n::defaultLanguageCodeForSystem()
{
    // 用 QLocale::system() 而不是 QLocale()：
    // 前者读的是系统区域设置，后者是"用户默认"，在部分环境下会不一致。
    const QLocale locale = QLocale::system();
    const QLocale::Language lang = locale.language();

    switch (lang) {
    case QLocale::Chinese:
        return QStringLiteral("zh-CN");
    case QLocale::Japanese:
        return QStringLiteral("ja-JP");
    case QLocale::English:
        return QStringLiteral("en-US");
    default:
        // 不支持的系统语言统一回退到英文。
        // 为什么不回退中文：本程序的界面文案中文最全，
        // 但对一个非中日的用户来说，英文的可读性通常更高。
        return QStringLiteral("en-US");
    }
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------
int I18n::loadFromDirectory(const QString &directory)
{
    m_directory = directory;
    m_tables.clear();

    if (directory.isEmpty() || !QFileInfo::exists(directory)) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("界面语言目录不存在：%1，将使用内置回退文案")
                    .arg(directory));
        return 0;
    }

    // 加载顺序故意固定：先中文（作为回退基准），再英文，再日文。
    // 后面的语言不会覆盖前面的（各存各的表），所以顺序只影响日志可读性。
    const QStringList codes = {
        QStringLiteral("zh-CN"),
        QStringLiteral("en-US"),
        QStringLiteral("ja-JP")
    };

    int loaded = 0;
    for (const QString &code : codes) {
        const QString path = QDir(directory).filePath(code + QStringLiteral(".json"));
        if (!QFileInfo::exists(path)) {
            DC_INFO(LogCat::Ui,
                    QStringLiteral("界面语言文件缺失（可忽略）：%1").arg(code));
            continue;
        }

        QHash<QString, QString> table;
        if (loadLanguageFile(path, &table) && !table.isEmpty()) {
            m_tables.insert(code, table);
            ++loaded;
            DC_INFO(LogCat::Ui,
                    QStringLiteral("已加载界面语言 %1（%2 条文案）")
                        .arg(code)
                        .arg(table.size()));
        }
    }

    if (loaded == 0) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("没有加载到任何界面语言文件，"
                               "界面将显示文案 key。请检查安装是否完整。"));
    }

    return loaded;
}

bool I18n::loadLanguageFile(const QString &path, QHash<QString, QString> *target)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("无法打开界面语言文件：%1")
                    .arg(redactedFileName(path)));
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 语言文件损坏不该让程序起不来，只记日志并跳过。
        // 界面会通过"回退到中文 -> 回退到 key"继续工作。
        DC_WARN(LogCat::Ui,
                QStringLiteral("界面语言文件格式错误：%1（%2，偏移 %3）")
                    .arg(path, parseError.errorString())
                    .arg(parseError.offset));
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject strings = root.value(QStringLiteral("strings")).toObject();

    if (strings.isEmpty()) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("界面语言文件里没有 strings 段：%1").arg(path));
        return false;
    }

    for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
        // 只接受字符串值。数组/对象一律跳过，
        // 免得后面 text() 取出来是个空串却不知道为什么。
        if (it.value().isString()) {
            target->insert(it.key(), it.value().toString());
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 语言切换
// ---------------------------------------------------------------------------
Language I18n::language() const
{
    return m_language;
}

void I18n::setLanguage(Language language)
{
    // Auto 对界面无意义 —— 解析成系统语言。
    // 这样调用方可以直接把配置里的值传进来，不用先判断是不是 Auto。
    Language resolved = language;
    if (resolved == Language::Auto) {
        const QString code = defaultLanguageCodeForSystem();
        resolved = languageFromCode(code);
        if (resolved == Language::Auto) {
            resolved = Language::EnUS;
        }
    }

    if (resolved == m_language) {
        return;   // 没变就不发信号，避免界面无谓重建
    }

    m_language = resolved;
    DC_INFO(LogCat::Ui,
            QStringLiteral("界面语言切换为 %1").arg(languageCode()));

    emit languageChanged(m_language);
}

// ---------------------------------------------------------------------------
// 取文案
// ---------------------------------------------------------------------------
QString I18n::text(const QString &key) const
{
    if (key.isEmpty()) {
        return QString();
    }

    // 1) 当前语言
    const QString code = languageCode();
    auto tableIt = m_tables.constFind(code);
    if (tableIt != m_tables.constEnd()) {
        auto valueIt = tableIt->constFind(key);
        if (valueIt != tableIt->constEnd() && !valueIt->isEmpty()) {
            return *valueIt;
        }
    }

    // 2) 回退中文
    //    为什么回退中文而不是英文：中文是本项目的一手语言，
    //    覆盖率最高。看到一个中文句子，用户至少知道功能是什么；
    //    看到 "dialog.permission.title" 就完全不知道了。
    if (code != QLatin1String("zh-CN")) {
        auto zhIt = m_tables.constFind(QStringLiteral("zh-CN"));
        if (zhIt != m_tables.constEnd()) {
            auto valueIt = zhIt->constFind(key);
            if (valueIt != zhIt->constEnd() && !valueIt->isEmpty()) {
                return *valueIt;
            }
        }
    }

    // 3) 回退 key 本身
    //    这是有意的：开发期能立刻看出漏配了哪个 key。
    //    如果返回空串，界面上只会看到一个空白按钮，更难定位。
    return key;
}

QString I18n::text(const QString &key, const QHash<QString, QString> &args) const
{
    QString result = text(key);

    // 逐个替换 {name} 形式的占位符。
    // 注意占位符名必须转义后再进正则：参数名理论上可以是任意字符串，
    // 直接拼进正则会有元字符问题（虽然实际调用都传简单标识符）。
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        const QString placeholder = QStringLiteral("{%1}").arg(it.key());
        result.replace(placeholder, it.value());
    }

    return result;
}

QString I18n::text(const QString &key, const QString &k1, const QString &v1) const
{
    QHash<QString, QString> args;
    args.insert(k1, v1);
    return text(key, args);
}

QString I18n::text(const QString &key,
                   const QString &k1, const QString &v1,
                   const QString &k2, const QString &v2) const
{
    QHash<QString, QString> args;
    args.insert(k1, v1);
    args.insert(k2, v2);
    return text(key, args);
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------
bool I18n::hasKey(const QString &key) const
{
    const QString code = languageCode();
    auto tableIt = m_tables.constFind(code);
    if (tableIt == m_tables.constEnd()) {
        return false;
    }
    auto valueIt = tableIt->constFind(key);
    return valueIt != tableIt->constEnd() && !valueIt->isEmpty();
}

QStringList I18n::missingKeys() const
{
    QStringList missing;

    // 以中文为基准比对：中文有而当前语言没有的，就算缺失。
    // 为什么不以英文为基准：中文覆盖率最高，用它当基准漏检最少。
    auto zhIt = m_tables.constFind(QStringLiteral("zh-CN"));
    if (zhIt == m_tables.constEnd()) {
        return missing;   // 连中文都没加载，无法比对
    }

    const QString code = languageCode();
    auto currentIt = m_tables.constFind(code);
    if (currentIt == m_tables.constEnd()) {
        return zhIt->keys();   // 当前语言整份都没有
    }

    for (auto it = zhIt->constBegin(); it != zhIt->constEnd(); ++it) {
        if (!currentIt->contains(it.key()) || currentIt->value(it.key()).isEmpty()) {
            missing << it.key();
        }
    }

    missing.sort();
    return missing;
}

QStringList I18n::loadedLanguages() const
{
    QStringList codes = m_tables.keys();
    codes.sort();
    return codes;
}

} // namespace dc
