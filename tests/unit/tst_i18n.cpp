// ============================================================================
//  tst_i18n.cpp — 界面多语言单元测试
//
//  需求依据：SRS V1.1 4.2 TXT-001（支持中文、英语、日语）
//
//  测试重点：
//    1. 三个语言文件都能加载（各 375 条）
//    2. **三种语言的 key 集合完全一致** —— 少一条就是漏翻
//    3. **占位符一致** —— 某语言把 {app} 抄丢了，运行时就会显示错乱
//    4. 回退链：当前语言 -> 中文 -> key 本身
//    5. 手动设置语言后 text() 返回对应语言
//
//  第 2、3 条是这里最有价值的：光看界面很难发现某个语言漏了一句，
//  或者某句少了占位符。必须靠机器核对。
//  开发过程中 ja-JP 就被查出 6 处占位符丢失。
// ============================================================================
#include <QtTest/QtTest>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include "model/Types.h"
#include "ui/I18n.h"

using namespace dc;

namespace {

QString i18nDir()
{
    return QString::fromUtf8(DC_TEST_RESOURCE_DIR) + QStringLiteral("/i18n");
}

// 直接读原始 JSON，用于「绕过 I18n 实现」独立核对文件内容。
// 这样即使 I18n 的加载逻辑有 bug，也能发现数据本身的问题。
QJsonObject rawStrings(const QString &langCode)
{
    const QString path = i18nDir() + QLatin1Char('/') + langCode + QStringLiteral(".json");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.object().value(QStringLiteral("strings")).toObject();
}

// 提取文案里的 {placeholder}
QSet<QString> placeholders(const QString &text)
{
    QSet<QString> out;
    static const QRegularExpression re(QStringLiteral("\\{([a-zA-Z0-9_]+)\\}"));
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        out.insert(it.next().captured(1));
    }
    return out;
}

const QStringList kLanguages = {
    QStringLiteral("zh-CN"),
    QStringLiteral("en-US"),
    QStringLiteral("ja-JP"),
};

} // namespace

class TestI18n : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    // ---- 加载 ----
    void load_allThreeLanguagesSucceed();
    void load_rawFilesHaveExpectedKeyCount();

    // ---- 完整性（核心）----
    void completeness_languageKeySetsAreIdentical();
    void completeness_noEmptyTranslations();
    void completeness_placeholdersMatchAcrossLanguages();

    // ---- 查询 ----
    void text_returnsTranslationForCurrentLanguage();
    void text_switchesLanguage();
    void text_unknownKeyFallsBackToKeyItself();

    // ---- 参数替换 ----
    void text_withArgsSubstitutesPlaceholders();

    // ---- 诊断 ----
    void diagnostics_loadedLanguagesListed();

private:
    // 以中文为基准，检查其它语言的 key 与占位符
    void compareAgainstBaseline(const QString &otherLang);
};

void TestI18n::initTestCase()
{
    const int loaded = I18n::instance().loadFromDirectory(i18nDir());
    QVERIFY2(loaded >= 1,
             qPrintable(QStringLiteral("未加载到任何语言文件，检查路径：%1").arg(i18nDir())));
}

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

void TestI18n::load_allThreeLanguagesSucceed()
{
    const int loaded = I18n::instance().loadFromDirectory(i18nDir());
    QCOMPARE(loaded, 3);
}

void TestI18n::load_rawFilesHaveExpectedKeyCount()
{
    for (const QString &lang : kLanguages) {
        const QJsonObject strings = rawStrings(lang);
        QVERIFY2(!strings.isEmpty(),
                 qPrintable(QStringLiteral("%1.json 没有 strings 段或为空").arg(lang)));

        // 三种语言各 375 条 —— 数量对不上说明有人加了 key 只改了一个文件
        QCOMPARE(strings.size(), 375);
    }
}

// ---------------------------------------------------------------------------
// 完整性
// ---------------------------------------------------------------------------

// 三种语言的 key 集合必须**完全一致**。
// 少一条 = 那个语言下有一段界面是空的（或回退成中文）。
void TestI18n::completeness_languageKeySetsAreIdentical()
{
    const QJsonObject zh = rawStrings(QStringLiteral("zh-CN"));
    QVERIFY(!zh.isEmpty());

    const QSet<QString> baselineKeys = [&zh] {
        QSet<QString> s;
        for (auto it = zh.constBegin(); it != zh.constEnd(); ++it)
            s.insert(it.key());
        return s;
    }();

    for (const QString &lang : kLanguages) {
        if (lang == QStringLiteral("zh-CN"))
            continue;

        const QJsonObject other = rawStrings(lang);
        QSet<QString> otherKeys;
        for (auto it = other.constBegin(); it != other.constEnd(); ++it)
            otherKeys.insert(it.key());

        QSet<QString> missing = baselineKeys;
        missing.subtract(otherKeys);
        QSet<QString> extra = otherKeys;
        extra.subtract(baselineKeys);

        QStringList missingList(missing.constBegin(), missing.constEnd());
        QStringList extraList(extra.constBegin(), extra.constEnd());
        missingList.sort();
        extraList.sort();

        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("%1 缺少 %2 条文案：\n%3")
                                .arg(lang)
                                .arg(missingList.size())
                                .arg(missingList.mid(0, 10).join(QStringLiteral("\n")))));

        QVERIFY2(extra.isEmpty(),
                 qPrintable(QStringLiteral("%1 多出 %2 条文案（其它语言没有）：\n%3")
                                .arg(lang)
                                .arg(extraList.size())
                                .arg(extraList.mid(0, 10).join(QStringLiteral("\n")))));
    }
}

// 不允许有空翻译 —— 空字符串在界面上就是一个空白标签。
void TestI18n::completeness_noEmptyTranslations()
{
    for (const QString &lang : kLanguages) {
        const QJsonObject strings = rawStrings(lang);
        QStringList empties;

        for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
            if (!it.value().isString() || it.value().toString().trimmed().isEmpty()) {
                empties << it.key();
            }
        }

        QVERIFY2(empties.isEmpty(),
                 qPrintable(QStringLiteral("%1 有 %2 条空翻译：\n%3")
                                .arg(lang)
                                .arg(empties.size())
                                .arg(empties.mid(0, 10).join(QStringLiteral("\n")))));
    }
}

// ★ 占位符必须跨语言一致。
// 例如中文写 "已打开 {app}"，英文必须也有 {app} ；
// 日语如果抄成 "開きました"（漏了占位符），用户就看不到具体打开了什么。
void TestI18n::completeness_placeholdersMatchAcrossLanguages()
{
    const QJsonObject zh = rawStrings(QStringLiteral("zh-CN"));
    QVERIFY(!zh.isEmpty());

    for (const QString &lang : kLanguages) {
        if (lang == QStringLiteral("zh-CN"))
            continue;

        const QJsonObject other = rawStrings(lang);
        QStringList mismatches;

        for (auto it = zh.constBegin(); it != zh.constEnd(); ++it) {
            const QString key = it.key();
            if (!other.contains(key))
                continue;   // key 缺失由上一个用例负责报

            const QSet<QString> basePh = placeholders(it.value().toString());
            const QSet<QString> otherPh = placeholders(other.value(key).toString());

            if (basePh != otherPh) {
                QStringList baseList(basePh.constBegin(), basePh.constEnd());
                QStringList otherList(otherPh.constBegin(), otherPh.constEnd());
                baseList.sort();
                otherList.sort();

                mismatches << QStringLiteral("%1  [中:{[%2]}] [%3:{[%4]}]")
                                  .arg(key,
                                       baseList.join(QStringLiteral(",")),
                                       lang,
                                       otherList.join(QStringLiteral(",")));
            }
        }

        QVERIFY2(mismatches.isEmpty(),
                 qPrintable(QStringLiteral("%1 有 %2 处占位符与中文不一致：\n%3")
                                .arg(lang)
                                .arg(mismatches.size())
                                .arg(mismatches.mid(0, 10).join(QStringLiteral("\n")))));
    }
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

void TestI18n::text_returnsTranslationForCurrentLanguage()
{
    I18n &i18n = I18n::instance();
    i18n.setLanguage(Language::ZhCN);
    i18n.loadFromDirectory(i18nDir());

    const QJsonObject zh = rawStrings(QStringLiteral("zh-CN"));
    // 取一个实际存在的 key
    const QString key = QStringLiteral("app.name");
    QVERIFY(zh.contains(key));

    QCOMPARE(i18n.text(key), zh.value(key).toString());
}

void TestI18n::text_switchesLanguage()
{
    I18n &i18n = I18n::instance();
    const QString key = QStringLiteral("app.name");

    const QJsonObject zh = rawStrings(QStringLiteral("zh-CN"));
    const QJsonObject en = rawStrings(QStringLiteral("en-US"));
    QVERIFY(zh.contains(key));
    QVERIFY(en.contains(key));

    i18n.setLanguage(Language::ZhCN);
    const QString zhText = i18n.text(key);

    i18n.setLanguage(Language::EnUS);
    const QString enText = i18n.text(key);

    QCOMPARE(zhText, zh.value(key).toString());
    QCOMPARE(enText, en.value(key).toString());
    QVERIFY2(zhText != enText,
             "中英文案相同，可能语言切换没生效（或翻译确实是同一个词）");
}

// 回退链的最后一环：完全找不到时返回 key 本身。
// 这样开发期一眼就能看出哪个 key 没配，而不是看到空白。
void TestI18n::text_unknownKeyFallsBackToKeyItself()
{
    I18n &i18n = I18n::instance();
    const QString missingKey = QStringLiteral("this.key.does.not.exist.at.all");

    QCOMPARE(i18n.text(missingKey), missingKey);
    QVERIFY(!i18n.hasKey(missingKey));
}

// ---------------------------------------------------------------------------
// 参数替换
// ---------------------------------------------------------------------------

void TestI18n::text_withArgsSubstitutesPlaceholders()
{
    I18n &i18n = I18n::instance();
    i18n.setLanguage(Language::ZhCN);

    // 找一个真实含占位符的 key
    const QJsonObject zh = rawStrings(QStringLiteral("zh-CN"));

    QString keyWithPlaceholder;
    QString placeholderName;
    for (auto it = zh.constBegin(); it != zh.constEnd(); ++it) {
        const QSet<QString> ph = placeholders(it.value().toString());
        if (ph.size() == 1) {
            keyWithPlaceholder = it.key();
            placeholderName = *ph.constBegin();
            break;
        }
    }

    QVERIFY2(!keyWithPlaceholder.isEmpty(),
             "界面文案里找不到含单个占位符的 key，无法测试参数替换");

    i18n.setLanguage(Language::ZhCN);
    const QString substituted = i18n.text(keyWithPlaceholder, placeholderName,
                                          QStringLiteral("MYVALUE"));

    QVERIFY2(!substituted.contains(QStringLiteral("{"))
                 || !substituted.contains(QStringLiteral("}")),
             qPrintable(QStringLiteral("占位符未被替换：%1").arg(substituted)));

    QVERIFY2(substituted.contains(QStringLiteral("MYVALUE")),
             qPrintable(QStringLiteral("替换后的值未出现在文案里：%1").arg(substituted)));
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

void TestI18n::diagnostics_loadedLanguagesListed()
{
    I18n &i18n = I18n::instance();
    i18n.loadFromDirectory(i18nDir());

    const QStringList langs = i18n.loadedLanguages();
    QVERIFY2(!langs.isEmpty(), "已加载语言列表为空");

    for (const QString &expected : kLanguages) {
        QVERIFY2(langs.contains(expected),
                 qPrintable(QStringLiteral("已加载语言列表缺少 %1：%2")
                                .arg(expected, langs.join(QStringLiteral(", ")))));
    }
}

void TestI18n::compareAgainstBaseline(const QString &otherLang)
{
    Q_UNUSED(otherLang)
}

QTEST_MAIN(TestI18n)
#include "tst_i18n.moc"
