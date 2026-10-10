// ============================================================================
//  CommandRepository.cpp — 规则仓库实现
//
//  需求依据：SRS V1.1 4.6、TXT-005、7.4
// ============================================================================
#include "engine/CommandRepository.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace dc {

// ---------------------------------------------------------------------------
//  RuleIssue::format
// ---------------------------------------------------------------------------
QString RuleIssue::format() const
{
    QString level;
    switch (severity) {
    case Severity::Info:    level = QStringLiteral("提示"); break;
    case Severity::Warning: level = QStringLiteral("警告"); break;
    case Severity::Error:   level = QStringLiteral("错误"); break;
    }

    if (ruleId.isEmpty())
        return QStringLiteral("[%1] %2").arg(level, message);

    return QStringLiteral("[%1] 规则 %2：%3").arg(level, ruleId, message);
}

bool RuleLoadResult::hasErrors() const
{
    for (const RuleIssue &issue : issues) {
        if (issue.severity == RuleIssue::Severity::Error)
            return true;
    }
    return false;
}

QStringList RuleLoadResult::errorMessages() const
{
    QStringList out;
    for (const RuleIssue &issue : issues) {
        if (issue.severity == RuleIssue::Severity::Error)
            out.append(issue.format());
    }
    return out;
}

// ===========================================================================
//  CommandRepository
// ===========================================================================
CommandRepository::CommandRepository(QObject *parent)
    : QObject(parent)
{
}

CommandRepository::~CommandRepository() = default;

// ---------------------------------------------------------------------------
//  编译：把含 {slot} 的模式串编译成正则
//
//  转换规则：
//    "打开{app}"        ->  ^打开([^\s]{1,32})$
//    "在{dir}打开{file}" ->  ^在([^\s]{1,64})打开([^\s]{1,128})$
//    "\{literal\}"      ->  转义为字面量 { 与 }
//
//  为什么捕获组用 [^\s]{1,N} 而不是 .* ：
//    命令场景中参数不该包含空白（否则「打开 A 和 B」会把「A 和 B」
//    整体当成一个参数）。限制为「非空白且长度受限」能有效降低误匹配，
//    同时也满足 SRS 7.3「所有外部输入均需校验」的意图。
//
//  长度上限按参数名做差异化，避免 {file} 这类路径参数被截断。
// ---------------------------------------------------------------------------
void CommandRepository::compileRule(CommandRule &rule) const
{
    rule.compiledPatterns.clear();
    rule.patternSlotOrders.clear();

    // 匹配两种形式：
    //   \{name\}  转义形式 —— 表示「字面量花括号里的内容」，不当作占位符
    //   {name}    正常占位符
    // 用一条正则一次扫完，避免两轮替换导致的边界问题。
    static const QRegularExpression slotRe(
        QStringLiteral("\\\\\\{(.*?)\\\\\\}")               // 转义形式
            + QStringLiteral("|\\{([A-Za-z_][A-Za-z0-9_]*)\\}")   // 正常占位符
    );

    for (const QString &pattern : rule.patterns) {
        QString regexBody;
        QStringList slotOrder;

        int lastEnd = 0;
        auto it = slotRe.globalMatch(pattern);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();

            // 前面的字面量部分：整体转义
            const QString literal = pattern.mid(lastEnd, m.capturedStart() - lastEnd);
            regexBody += QRegularExpression::escape(literal);

            if (!m.captured(1).isNull()) {
                // \{...\} 字面量
                regexBody += QRegularExpression::escape(QStringLiteral("{")
                                                        + m.captured(1)
                                                        + QStringLiteral("}"));
            } else {
                const QString slot = m.captured(2);
                slotOrder.append(slot);

                // 按槽名决定长度上限：
                //   file / path / dir  -> 路径可能较长
                //   text / query       -> 中等
                //   其他               -> 较短
                int maxLen = 32;
                if (slot.contains(QStringLiteral("file"), Qt::CaseInsensitive)
                    || slot.contains(QStringLiteral("path"), Qt::CaseInsensitive)
                    || slot.contains(QStringLiteral("dir"), Qt::CaseInsensitive)) {
                    maxLen = 260;   // 贴近 Windows MAX_PATH
                } else if (slot.contains(QStringLiteral("text"), Qt::CaseInsensitive)
                           || slot.contains(QStringLiteral("query"), Qt::CaseInsensitive)
                           || slot.contains(QStringLiteral("sentence"), Qt::CaseInsensitive)) {
                    maxLen = 512;
                }

                // ---- 槽的匹配表达式 ----
                //
                //  这里踩过一个影响面很大的坑，务必看明白再改。
                //
                //  最早的实现是 "([^\\s]{1,N})"，即「非空白字符 1..N 个」。
                //  它有个致命问题：**不允许格子内部有空格**。
                //
                //  规则写的是「搜索{query}」，用户自然会说「搜索 今天天气」
                //  （动词后加一个空格）。此时正则变成
                //      ^搜索([^\s]{1,512})$
                //  中间多了个空格，**整条模式匹配失败**，规则形同虚设。
                //  受影响的正是最常用的几条：
                //      搜索{query}       -> "搜索 今天天气"  匹配不上
                //      翻译{text}        -> "翻译 hello world" 匹配不上
                //      打开{app}         -> "打开 chrome"     匹配不上
                //  而用户完全不知道为什么「照着提示说话」却没反应。
                //
                //  修法：把槽放宽成「一个或多个以空白分隔的词组」，
                //  并在两端允许空白被吸收（这样「搜索 天气」和「搜索天气」
                //  都能匹配，且捕获到的值已经去掉了外侧空格）。
                //
                //     \s*                      允许动词后跟空格
                //     ( ... )                  捕获组（保持组号 = 槽序号 + 1）
                //     [^\s]{1,N}(?:\s+[^\s]{1,N})*   词 + 可选后续词
                //     \s*                      允许结尾空格
                //
                //  注意：不能简单写成 "([\s\S]{1,N})"（任意字符）——
                //  那会让槽贪婪吃掉后面紧跟的字面量，例如
                //  「打开{folder}文件夹」会把 "下载文件夹" 整个吞进 folder，
                //  导致后面的「文件夹」再也匹配不上。
                regexBody += QStringLiteral("\\s*([^\\s]{1,%1}(?:\\s+[^\\s]{1,%1})*)\\s*")
                                 .arg(maxLen);
            }

            lastEnd = m.capturedEnd();
        }
        // 尾部字面量
        regexBody += QRegularExpression::escape(pattern.mid(lastEnd));

        QRegularExpression re(QStringLiteral("^") + regexBody + QStringLiteral("$"),
                              QRegularExpression::CaseInsensitiveOption
                                  | QRegularExpression::UseUnicodePropertiesOption);

        if (re.isValid()) {
            rule.compiledPatterns.append(re);
            rule.patternSlotOrders.append(slotOrder.join(QLatin1Char(',')));
        } else {
            DC_WARN(LogCat::Command,
                    QStringLiteral("规则 %1 的模式「%2」编译失败：%3")
                        .arg(rule.id, pattern, re.errorString()));
            // 保持索引对齐：放入一个永不匹配的正则
            rule.compiledPatterns.append(
                QRegularExpression(QStringLiteral("(?!)")));
            rule.patternSlotOrders.append(QString());
        }
    }
}

// ---------------------------------------------------------------------------
//  加载
// ---------------------------------------------------------------------------
RuleLoadResult CommandRepository::loadFromDirectory(const QString &directory)
{
    QDir dir(directory);
    if (!dir.exists()) {
        RuleLoadResult result;
        result.issues.append(RuleIssue{
            RuleIssue::Severity::Error, {},
            QStringLiteral("规则目录不存在：%1").arg(directory) });
        return result;
    }

    const QFileInfoList files = dir.entryInfoList(
        { QStringLiteral("*.json") }, QDir::Files, QDir::Name);

    QStringList paths;
    for (const QFileInfo &fi : files)
        paths.append(fi.absoluteFilePath());

    return loadFromFiles(paths);
}

RuleLoadResult CommandRepository::loadFromFiles(const QStringList &files)
{
    RuleLoadResult result;
    result.sourceFiles = files;

    // 是否已有规则：有多文件时，后加载的文件覆盖同名规则。
    // 这样用户可以在自定义文件里覆盖内置规则而无需改原始文件。
    const bool isFirstLoad = m_rules.isEmpty();

    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.issues.append(RuleIssue{
                RuleIssue::Severity::Error, {},
                QStringLiteral("无法读取规则文件：%1（%2）")
                    .arg(path, file.errorString()) });
            continue;
        }

        const QByteArray raw = file.readAll();
        file.close();

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);

        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            result.issues.append(RuleIssue{
                RuleIssue::Severity::Error, {},
                QStringLiteral("规则文件格式有误：%1（%2，位置 %3）")
                    .arg(QFileInfo(path).fileName(),
                         parseError.errorString())
                    .arg(parseError.offset) });
            continue;
        }

        QString setError;
        const CommandRuleSet ruleSet = CommandRuleSet::fromJson(doc.object(), &setError);

        if (!setError.isEmpty()) {
            result.issues.append(RuleIssue{
                RuleIssue::Severity::Warning, {}, setError });
        }

        // 合并别名表
        for (auto tableIt = ruleSet.aliases.constBegin();
             tableIt != ruleSet.aliases.constEnd(); ++tableIt) {
            // 后加载的覆盖先加载的同名条目（用户文件优先）
            QMap<QString, QString> &target = m_aliases[tableIt.key()];
            for (auto entryIt = tableIt.value().constBegin();
                 entryIt != tableIt.value().constEnd(); ++entryIt) {
                target[entryIt.key()] = entryIt.value();
            }
        }
        result.loadedAliasTables = m_aliases.size();

        // 逐条规则校验并编译
        for (CommandRule rule : ruleSet.rules) {
            const QString validationError = rule.validationError();
            if (!validationError.isEmpty()) {
                result.issues.append(RuleIssue{
                    RuleIssue::Severity::Error, rule.id,
                    QStringLiteral("%1（来源：%2）")
                        .arg(validationError, QFileInfo(path).fileName()) });
                continue;   // 无效规则直接丢弃，避免运行期出现难以排查的匹配异常
            }

            // 检查别名表引用是否存在
            for (const ParameterTemplate &p : rule.parameters) {
                if (p.source == ParameterSource::AliasMap
                    && !m_aliases.contains(p.value)) {
                    result.issues.append(RuleIssue{
                        RuleIssue::Severity::Warning, rule.id,
                        QStringLiteral("参数 %1 引用了未定义的别名表「%2」，"
                                       "该参数将无法归一化。")
                            .arg(p.name, p.value) });
                }
            }

            compileRule(rule);

            if (m_rules.contains(rule.id) && !isFirstLoad) {
                result.issues.append(RuleIssue{
                    RuleIssue::Severity::Info, rule.id,
                    QStringLiteral("规则被后加载的文件覆盖（来源：%1）")
                        .arg(QFileInfo(path).fileName()) });
            }

            m_rules.insert(rule.id, rule);
        }
    }

    // 加载完成后再做全局校验（跨规则冲突等）
    const QVector<RuleIssue> globalIssues = validate();
    result.issues.append(globalIssues);
    result.loadedRuleCount = m_rules.size();
    result.ok = !result.hasErrors();

    ++m_revision;

    DC_INFO(LogCat::Command,
            QStringLiteral("规则加载完成：%1 条规则，%2 张别名表，来自 %3 个文件，"
                           "问题 %4 条")
                .arg(result.loadedRuleCount)
                .arg(result.loadedAliasTables)
                .arg(files.size())
                .arg(result.issues.size()));

    emit loadIssues(result.issues);
    emit rulesChanged();

    return result;
}

// ---------------------------------------------------------------------------
//  修改
// ---------------------------------------------------------------------------
void CommandRepository::addRule(const CommandRule &rule)
{
    CommandRule copy = rule;
    compileRule(copy);
    m_rules.insert(copy.id, copy);

    ++m_revision;
    emit rulesChanged();
}

void CommandRepository::removeRule(const QString &ruleId)
{
    if (m_rules.remove(ruleId) > 0) {
        ++m_revision;
        emit rulesChanged();
    }
}

void CommandRepository::clear()
{
    m_rules.clear();
    m_aliases.clear();

    ++m_revision;
    emit rulesChanged();
}

// ---------------------------------------------------------------------------
//  查询
// ---------------------------------------------------------------------------
QVector<CommandRule> CommandRepository::rules() const
{
    // 按优先级降序、id 升序输出，保证顺序稳定（便于测试）
    QVector<CommandRule> out;
    out.reserve(m_rules.size());
    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it)
        out.append(it.value());

    std::sort(out.begin(), out.end(), [](const CommandRule &a, const CommandRule &b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        return a.id < b.id;
    });

    return out;
}

QVector<CommandRule> CommandRepository::enabledRules() const
{
    QVector<CommandRule> out;
    for (const CommandRule &rule : rules()) {
        if (rule.enabled)
            out.append(rule);
    }
    return out;
}

CommandRule CommandRepository::ruleById(const QString &ruleId) const
{
    return m_rules.value(ruleId);
}

bool CommandRepository::contains(const QString &ruleId) const
{
    return m_rules.contains(ruleId);
}

int CommandRepository::count() const
{
    return m_rules.size();
}

QVector<CommandRule> CommandRepository::rulesByIntent(const QString &intent) const
{
    QVector<CommandRule> out;
    for (const CommandRule &rule : rules()) {
        if (rule.intent == intent)
            out.append(rule);
    }
    return out;
}

// ---------------------------------------------------------------------------
//  别名
// ---------------------------------------------------------------------------
QString CommandRepository::normalizeAlias(const QString &tableName,
                                         const QString &value) const
{
    if (!m_aliases.contains(tableName))
        return value;

    const QMap<QString, QString> &table = m_aliases.value(tableName);

    // 1) 精确匹配（大小写不敏感）
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        if (it.key().compare(value, Qt::CaseInsensitive) == 0)
            return it.value();
    }

    // 2) 归一化匹配：去空白后比较
    const QString stripped = value.simplified();
    for (auto it = table.constBegin(); it != table.constEnd(); ++it) {
        if (it.key().simplified().compare(stripped, Qt::CaseInsensitive) == 0)
            return it.value();
    }

    return value;
}

bool CommandRepository::hasAliasTable(const QString &tableName) const
{
    return m_aliases.contains(tableName);
}

QStringList CommandRepository::aliasTables() const
{
    return m_aliases.keys();
}

QMap<QString, QString> CommandRepository::aliasTable(const QString &tableName) const
{
    return m_aliases.value(tableName);
}

// ---------------------------------------------------------------------------
//  校验
// ---------------------------------------------------------------------------
QVector<RuleIssue> CommandRepository::validate() const
{
    QVector<RuleIssue> issues;

    // ---- 单规则校验（加载时已做，这里针对内存中后加入的规则再查一遍）----
    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it) {
        const CommandRule &rule = it.value();
        const QString err = rule.validationError();
        if (!err.isEmpty()) {
            issues.append(RuleIssue{ RuleIssue::Severity::Error, rule.id, err });
        }

        // 高风险规则必须要求确认，否则是配置疏漏（SRS 4.4）
        if (rule.riskLevel == RiskLevel::High && !rule.requiresConfirmation) {
            issues.append(RuleIssue{
                RuleIssue::Severity::Warning, rule.id,
                QStringLiteral("高风险规则未设置 confirmation=true，"
                               "将在运行时强制要求授权。") });
        }

        // 禁止级规则不应出现在可用规则集里
        if (rule.riskLevel == RiskLevel::Forbidden && rule.enabled) {
            issues.append(RuleIssue{
                RuleIssue::Severity::Error, rule.id,
                QStringLiteral("风险等级为 forbidden 的规则不应启用（SRS 4.4：不提供）。") });
        }

        // 模式串中的占位符顺序必须与参数声明一致（数量上）
        for (int i = 0; i < rule.compiledPatterns.size(); ++i) {
            const QString slotOrderText = rule.patternSlotOrders.value(i);
            const QStringList slotNames = slotOrderText.isEmpty()
                ? QStringList()
                : slotOrderText.split(QLatin1Char(','), Qt::SkipEmptyParts);

            int declaredSlotCount = 0;
            for (const ParameterTemplate &p : rule.parameters) {
                if (p.source == ParameterSource::Slot)
                    ++declaredSlotCount;
            }

            // 允许模式串使用 _ 前缀的忽略槽，因此只校验「非忽略槽」数量，
            // 这里做宽松检查：捕获组数不得少于非忽略槽数
            int nonIgnored = 0;
            for (const QString &s : slotNames) {
                if (!s.startsWith(QLatin1Char('_')))
                    ++nonIgnored;
            }

            if (nonIgnored > declaredSlotCount) {
                issues.append(RuleIssue{
                    RuleIssue::Severity::Warning, rule.id,
                    QStringLiteral("模式「%1」有 %2 个占位符，"
                                   "但只声明了 %3 个槽参数。")
                        .arg(rule.patterns.value(i))
                        .arg(nonIgnored)
                        .arg(declaredSlotCount) });
            }
        }
    }

    // ---- 冲突检测 ----
    const QVector<RuleConflict> conflicts = detectConflicts();
    for (const RuleConflict &c : conflicts) {
        issues.append(RuleIssue{
            c.samePriority ? RuleIssue::Severity::Warning : RuleIssue::Severity::Info,
            c.ruleIdB,
            c.description });
    }

    return issues;
}

// ---------------------------------------------------------------------------
//  冲突检测
//
//  判定方式：
//    对每个「语言 + 模式串指纹」做归并，指纹相同即潜在冲突。
//    指纹计算会：
//      1. 去掉空白
//      2. 大小写折叠
//      3. 把 {slot} 统一为 {?} —— 因为 {a} 与 {b} 结构上等价
//    语言不同的规则不冲突（它们面向不同语言的输入）。
// ---------------------------------------------------------------------------
QString CommandRepository::patternFingerprint(const CommandRule &rule,
                                             const QString &pattern)
{
    QString fp = pattern.simplified().toLower();

    // 归一化占位符
    static const QRegularExpression slotRe(
        QStringLiteral("\\{[A-Za-z_][A-Za-z0-9_]*\\}"));
    fp.replace(slotRe, QStringLiteral("{?}"));

    // 语言前缀：Auto 视为「通用」，与任何语言都可能冲突
    const QString langPrefix =
        rule.language == Language::Auto ? QStringLiteral("*") : languageToCode(rule.language);

    return langPrefix + QStringLiteral("|") + fp;
}

QVector<RuleConflict> CommandRepository::detectConflicts() const
{
    QVector<RuleConflict> conflicts;

    // fingerprint -> [(ruleId, pattern, priority)]
    struct Entry {
        QString ruleId;
        QString pattern;
        int priority;
        Language language;
    };
    QHash<QString, QVector<Entry>> buckets;

    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it) {
        const CommandRule &rule = it.value();
        if (!rule.enabled)
            continue;

        for (const QString &pattern : rule.patterns) {
            const QString fp = patternFingerprint(rule, pattern);
            buckets[fp].append(Entry{ rule.id, pattern, rule.priority, rule.language });
        }
    }

    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        QVector<Entry> entries = it.value();
        if (entries.size() < 2)
            continue;

        for (int i = 0; i < entries.size(); ++i) {
            for (int j = i + 1; j < entries.size(); ++j) {
                const Entry &a = entries.at(i);
                const Entry &b = entries.at(j);

                // 语言不同的两条规则不算冲突，除非其中一条是 Auto（通用规则）
                const bool sameLanguage =
                    a.language == b.language
                    || a.language == Language::Auto
                    || b.language == Language::Auto;

                if (!sameLanguage)
                    continue;

                RuleConflict conflict;
                conflict.ruleIdA = a.ruleId;
                conflict.ruleIdB = b.ruleId;
                conflict.pattern = a.pattern;
                conflict.samePriority = (a.priority == b.priority);

                if (conflict.samePriority) {
                    conflict.description =
                        QStringLiteral("规则 %1 与 %2 的模式「%3」完全相同且优先级相同，"
                                       "匹配时会要求用户澄清（SRS 4.6）。")
                            .arg(a.ruleId, b.ruleId, a.pattern);
                } else {
                    conflict.description =
                        QStringLiteral("规则 %1（优先级 %2）与 %3（优先级 %4）的"
                                       "模式「%5」相同，将由优先级消解。")
                            .arg(a.ruleId).arg(a.priority)
                            .arg(b.ruleId).arg(b.priority)
                            .arg(a.pattern);
                }

                conflicts.append(conflict);
            }
        }
    }

    return conflicts;
}

// ---------------------------------------------------------------------------
//  统计
// ---------------------------------------------------------------------------
QHash<Language, int> CommandRepository::countByLanguage() const
{
    QHash<Language, int> counts;
    for (auto it = m_rules.constBegin(); it != m_rules.constEnd(); ++it)
        ++counts[it.value().language];
    return counts;
}

int CommandRepository::revision() const
{
    return m_revision;
}

} // namespace dc
