// ============================================================================
//  I18n.h — 界面多语言（中文 / 英语 / 日语）
//
//  需求依据：SRS V1.1 4.2 TXT-001「支持中文、英语、日语」
//
//  为什么不用 Qt 自带的 tr() + .ts/.qm 机制：
//    .ts 需要在构建流程里跑 lupdate/lrelease，产生 .qm 二进制文件。
//    对一个"轻量、可手工检查"的桌宠来说，这让翻译变成了构建期黑盒 ——
//    用户想改一个词的译法必须重新编译。
//
//    本实现改为读 JSON 语言文件（resources/i18n/zh-CN.json 等），
//    理由有三：
//      1. 翻译是纯数据，用户可以直接改，改完重启就生效
//      2. 缺翻译时能回退到中文而不是显示英文 key
//      3. 不需要在构建系统里加额外工具链（SRS 8.4：少依赖）
//
//  使用方式：
//      I18n::instance().text("dialog.permission.title")
//
//  约定：所有 key 用点分层级命名，与控制台/日志里的 key 一一对应，
//        便于用脚本核对三种语言是否漏翻。
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

namespace dc {

class I18n : public QObject
{
    Q_OBJECT

public:
    static I18n &instance();

    // -----------------------------------------------------------------------
    // 加载语言文件
    //
    //  directory 下应有 zh-CN.json / en-US.json / ja-JP.json。
    //  任一文件缺失或格式错误都不会抛错 —— 缺失的语言回退到中文，
    //  中文也缺失时返回 key 本身（便于开发期发现漏翻）。
    //
    //  返回实际成功加载的语言数量。
    // -----------------------------------------------------------------------
    int loadFromDirectory(const QString &directory);

    // 当前语言
    Language language() const;
    void setLanguage(Language language);
    QString languageCode() const;

    // -----------------------------------------------------------------------
    // 取文案
    //
    //  key 形如 "dialog.permission.title"。
    //  查不到时的回退顺序：当前语言 -> 中文 -> key 本身。
    //  最后回退到 key 是有意的：开发时一眼就能看出哪个 key 没配，
    //  而不是看到一个空白按钮。
    // -----------------------------------------------------------------------
    QString text(const QString &key) const;

    // 带参数替换：text("msg.opened", {"app", "Chrome"})
    // 文案里写 {app} 即可。
    QString text(const QString &key,
                 const QHash<QString, QString> &args) const;

    // 便捷形式，最多三个参数（覆盖绝大多数场景）
    QString text(const QString &key, const QString &k1, const QString &v1) const;
    QString text(const QString &key,
                 const QString &k1, const QString &v1,
                 const QString &k2, const QString &v2) const;

    // 该 key 在当前语言下是否有翻译（供"翻译完整度"诊断用）
    bool hasKey(const QString &key) const;

    // 当前语言缺失的 key 列表（诊断页展示，让用户知道哪些还是中文）
    QStringList missingKeys() const;

    // 已加载的语言代码列表
    QStringList loadedLanguages() const;

    // -----------------------------------------------------------------------
    // 界面语言 <-> Language 枚举
    //   Language::Auto 对界面无意义，这里统一按系统语言解析。
    // -----------------------------------------------------------------------
    static QString defaultLanguageCodeForSystem();

signals:
    // 语言切换完成，界面应重新取一遍文案
    void languageChanged(dc::Language language);

private:
    I18n();
    ~I18n() override;
    Q_DISABLE_COPY(I18n)

    bool loadLanguageFile(const QString &path, QHash<QString, QString> *target);

    // 代码 -> 文案表
    QHash<QString, QHash<QString, QString>> m_tables;

    Language m_language = Language::ZhCN;
    QString m_directory;
};

} // namespace dc
