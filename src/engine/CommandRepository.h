// ============================================================================
//  CommandRepository.h — 命令规则仓库（加载、校验、索引、冲突检测）
//
//  需求依据：SRS V1.1 第 4.6 节
//    - 支持命令匹配、参数提取、别名和优先级
//    - 对冲突规则提供配置校验
//    - 命令词条、提示文本和语言资源应与程序逻辑分离（TXT-005）
//
//  设计说明：
//    规则以 JSON 文件形式外置（resources/commands/*.json），
//    程序启动时加载、校验、编译为正则，之后只读访问。
//    这样用户可以直接编辑 JSON 定制命令，不需要重新编译程序。
// ============================================================================
#pragma once

#include "model/CommandRule.h"
#include "model/Types.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 规则集校验问题
// ---------------------------------------------------------------------------
struct RuleIssue {
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Warning;
    QString ruleId;
    QString message;

    QString format() const;
};

// ---------------------------------------------------------------------------
// 规则集加载结果
// ---------------------------------------------------------------------------
struct RuleLoadResult {
    bool ok = false;
    int loadedRuleCount = 0;
    int loadedAliasTables = 0;
    QStringList sourceFiles;
    QVector<RuleIssue> issues;

    bool hasErrors() const;
    QStringList errorMessages() const;
};

// ---------------------------------------------------------------------------
// 冲突描述（SRS 4.6：对冲突规则提供配置校验）
//
//  冲突定义：
//    两条规则的模式串在「规范化后完全一致」时构成「直接冲突」，
//    若两者优先级相同，解析时必然产生歧义（SRS 4.6 要求此时向用户澄清）。
//    若优先级不同，则高优先级胜出，但配置上仍值得提示。
// ---------------------------------------------------------------------------
struct RuleConflict {
    QString ruleIdA;
    QString ruleIdB;
    QString pattern;          // 冲突的模式串
    bool samePriority = false;   // true = 必然歧义；false = 靠优先级消解
    QString description;
};

// ---------------------------------------------------------------------------
// CommandRepository
// ---------------------------------------------------------------------------
class CommandRepository : public QObject
{
    Q_OBJECT

public:
    explicit CommandRepository(QObject *parent = nullptr);
    ~CommandRepository() override;

    // ---- 加载 --------------------------------------------------------
    // fromDirectory: 加载目录下所有 *.json 规则文件（按文件名排序，后者覆盖前者）
    // fromFiles    : 加载指定文件列表
    RuleLoadResult loadFromDirectory(const QString &directory);
    RuleLoadResult loadFromFiles(const QStringList &files);

    // 追加用户自定义规则（内存中，不写盘；写盘由 SettingsDialog 负责）
    void addRule(const CommandRule &rule);
    void removeRule(const QString &ruleId);

    // 清空全部规则
    void clear();

    // ---- 查询 --------------------------------------------------------
    QVector<CommandRule> rules() const;
    QVector<CommandRule> enabledRules() const;
    CommandRule ruleById(const QString &ruleId) const;
    bool contains(const QString &ruleId) const;
    int count() const;

    // 按意图查询（用于「这个意图有哪些规则」）
    QVector<CommandRule> rulesByIntent(const QString &intent) const;

    // ---- 别名 --------------------------------------------------------
    // 解析别名：normalizeAlias("浏览器") -> "browser"
    // 找不到时原样返回（表示该输入本身就是标准值）
    QString normalizeAlias(const QString &tableName, const QString &value) const;
    bool hasAliasTable(const QString &tableName) const;
    QStringList aliasTables() const;
    QMap<QString, QString> aliasTable(const QString &tableName) const;

    // ---- 校验 --------------------------------------------------------
    // 校验全部规则，返回问题列表
    QVector<RuleIssue> validate() const;

    // 检测规则冲突
    QVector<RuleConflict> detectConflicts() const;

    // ---- 统计 --------------------------------------------------------
    // 按语言统计规则数（用于确认中英日三语都覆盖到了）
    QHash<Language, int> countByLanguage() const;

    // 规则集版本号（每次修改自增，用于缓存失效判断）
    int revision() const;

signals:
    void rulesChanged();
    void loadIssues(const QVector<dc::RuleIssue> &issues);

private:
    // 编译规则中的模式串为正则
    void compileRule(CommandRule &rule) const;

    // 归一化模式串（用于冲突检测）
    static QString patternFingerprint(const CommandRule &rule, const QString &pattern);

    QMap<QString, CommandRule> m_rules;                  // id -> 规则
    QMap<QString, QMap<QString, QString>> m_aliases;     // 表名 -> (别名 -> 值)
    int m_revision = 0;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::RuleIssue)
Q_DECLARE_METATYPE(dc::RuleConflict)
