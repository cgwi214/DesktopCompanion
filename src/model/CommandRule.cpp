// ============================================================================
//  CommandRule.cpp — 命令规则序列化、编译与校验
//
//  需求依据：SRS V1.1 第 4.6 节
//    - 支持命令匹配、参数提取、别名和优先级
//    - 对冲突规则提供配置校验
//    - 所有命令执行前进行权限和参数校验
// ============================================================================
#include "model/CommandRule.h"

#include <QJsonArray>
#include <QSet>

namespace dc {

// ===========================================================================
//  ParameterSource
// ===========================================================================
QString parameterSourceToString(ParameterSource source)
{
    switch (source) {
    case ParameterSource::Constant: return QStringLiteral("constant");
    case ParameterSource::Slot:     return QStringLiteral("slot");
    case ParameterSource::AliasMap: return QStringLiteral("alias_map");
    }
    return QStringLiteral("constant");
}

ParameterSource parameterSourceFromString(const QString &text, bool *ok)
{
    const QString key = text.trimmed().toLower();
    if (key == QStringLiteral("constant")) {
        if (ok) *ok = true;
        return ParameterSource::Constant;
    }
    if (key == QStringLiteral("slot") || key == QStringLiteral("capture")) {
        if (ok) *ok = true;
        return ParameterSource::Slot;
    }
    if (key == QStringLiteral("alias_map") || key == QStringLiteral("alias")) {
        if (ok) *ok = true;
        return ParameterSource::AliasMap;
    }
    if (ok) *ok = false;
    return ParameterSource::Constant;
}

// ===========================================================================
//  ParameterTemplate
// ===========================================================================
QJsonObject ParameterTemplate::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("source")] = parameterSourceToString(source);
    if (source == ParameterSource::Constant && !value.isEmpty())
        obj[QStringLiteral("value")] = value;
    if (source == ParameterSource::Slot && !slotName.isEmpty())
        obj[QStringLiteral("slot")] = slotName;
    if (source == ParameterSource::AliasMap && !value.isEmpty())
        obj[QStringLiteral("table")] = value;
    if (!validationPattern.isEmpty())
        obj[QStringLiteral("validation")] = validationPattern;
    if (!required)
        obj[QStringLiteral("required")] = false;
    return obj;
}

ParameterTemplate ParameterTemplate::fromJson(const QJsonObject &obj)
{
    ParameterTemplate t;
    t.source = parameterSourceFromString(
        obj.value(QStringLiteral("source")).toString(QStringLiteral("constant")));
    t.value = obj.value(QStringLiteral("value")).toString();
    if (t.value.isEmpty())
        t.value = obj.value(QStringLiteral("table")).toString();   // alias_map 的写法
    t.slotName = obj.value(QStringLiteral("slot")).toString();
    t.validationPattern = obj.value(QStringLiteral("validation")).toString();
    t.required = obj.value(QStringLiteral("required")).toBool(true);
    return t;
}

// ===========================================================================
//  CommandRule
// ===========================================================================
bool CommandRule::isValid() const
{
    return validationError().isEmpty();
}

QString CommandRule::validationError() const
{
    if (id.trimmed().isEmpty())
        return QStringLiteral("规则缺少 id");
    if (intent.trimmed().isEmpty())
        return QStringLiteral("规则 %1 缺少 intent").arg(id);
    if (patterns.isEmpty())
        return QStringLiteral("规则 %1 未定义任何 patterns").arg(id);

    // 检查占位符与参数模板是否自洽：
    //   模式串里的 {slot} 必须能在 parameters 中找到（Slot 来源），
    //   否则规则解析时会出现「捕获到值却无处存放」的静默失败。
    QSet<QString> declaredSlots;
    for (const ParameterTemplate &p : parameters) {
        if (p.source == ParameterSource::Slot) {
            if (p.slotName.isEmpty())
                return QStringLiteral("规则 %1 的参数 %2 声明为 slot 但未指定 slot 名")
                           .arg(id, p.name);
            declaredSlots.insert(p.slotName);
        }
        if (p.source == ParameterSource::Constant && p.name.isEmpty())
            return QStringLiteral("规则 %1 存在未命名的常量参数").arg(id);
        if (p.source == ParameterSource::AliasMap && p.value.isEmpty())
            return QStringLiteral("规则 %1 的参数 %2 声明为 alias_map 但未指定别名表")
                       .arg(id, p.name);
        // 参数校验正则必须可编译，否则运行时静默失效（SRS 7.3 要求校验生效）
        if (!p.validationPattern.isEmpty()) {
            const QRegularExpression re(p.validationPattern);
            if (!re.isValid()) {
                return QStringLiteral("规则 %1 的参数 %2 校验正则无效：%3")
                           .arg(id, p.name, re.errorString());
            }
        }
    }

    // 校验每个模式串本身可编译，且占位符都能被参数模板承接
    for (const QString &pattern : patterns) {
        if (pattern.trimmed().isEmpty())
            return QStringLiteral("规则 %1 含空模式串").arg(id);

        static const QRegularExpression slotExtractor(QStringLiteral("\\{([A-Za-z_][A-Za-z0-9_]*)\\}"));
        auto it = slotExtractor.globalMatch(pattern);
        while (it.hasNext()) {
            const QString slot = it.next().captured(1);
            if (!declaredSlots.contains(slot)) {
                // 允许匿名丢弃槽（用下划线开头声明为忽略）
                if (!slot.startsWith(QLatin1Char('_'))) {
                    return QStringLiteral("规则 %1 的模式「%2」引用了未声明的占位符 {%3}")
                               .arg(id, pattern, slot);
                }
            }
        }
    }

    return QString();
}

QJsonObject CommandRule::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("id")] = id;
    if (language != Language::Auto)
        obj[QStringLiteral("language")] = languageToCode(language);
    obj[QStringLiteral("patterns")] = QJsonArray::fromStringList(patterns);
    obj[QStringLiteral("intent")] = intent;

    if (!parameters.isEmpty()) {
        QJsonObject params;
        for (const ParameterTemplate &p : parameters)
            params[p.name] = p.toJson();
        obj[QStringLiteral("parameters")] = params;
    }

    obj[QStringLiteral("risk_level")] = riskLevelToString(riskLevel);
    obj[QStringLiteral("confirmation")] = requiresConfirmation;
    if (priority != 0)
        obj[QStringLiteral("priority")] = priority;
    if (!enabled)
        obj[QStringLiteral("enabled")] = false;
    if (!category.isEmpty())
        obj[QStringLiteral("category")] = category;
    if (!description.isEmpty())
        obj[QStringLiteral("description")] = description;
    return obj;
}

CommandRule CommandRule::fromJson(const QJsonObject &obj, QString *error)
{
    CommandRule rule;
    rule.id = obj.value(QStringLiteral("id")).toString();
    rule.intent = obj.value(QStringLiteral("intent")).toString();

    const QString langCode = obj.value(QStringLiteral("language")).toString();
    rule.language = langCode.isEmpty() ? Language::Auto : languageFromCode(langCode);

    const QJsonArray patternArr = obj.value(QStringLiteral("patterns")).toArray();
    for (const QJsonValue &v : patternArr) {
        const QString p = v.toString();
        if (!p.isEmpty())
            rule.patterns.append(p);
    }

    const QJsonObject paramsObj = obj.value(QStringLiteral("parameters")).toObject();
    for (auto it = paramsObj.constBegin(); it != paramsObj.constEnd(); ++it) {
        ParameterTemplate t = ParameterTemplate::fromJson(it.value().toObject());
        t.name = it.key();
        rule.parameters.append(t);
    }

    const QString riskText = obj.value(QStringLiteral("risk_level")).toString();
    bool riskOk = false;
    rule.riskLevel = riskLevelFromString(riskText, &riskOk);
    if (!riskText.isEmpty() && !riskOk && error) {
        // 不静默忽略：未知风险等级已按最严格处理，同时上报给配置校验
        *error = QStringLiteral("规则 %1 的风险等级「%2」无法识别，已按 forbidden 处理")
                     .arg(rule.id, riskText);
    }

    rule.requiresConfirmation = obj.value(QStringLiteral("confirmation")).toBool(false);
    rule.priority = obj.value(QStringLiteral("priority")).toInt(0);
    rule.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
    rule.category = obj.value(QStringLiteral("category")).toString();
    rule.description = obj.value(QStringLiteral("description")).toString();

    return rule;
}

// ===========================================================================
//  CommandRuleSet
// ===========================================================================
QJsonObject CommandRuleSet::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("schema_version")] = schemaVersion;

    QJsonObject aliasRoot;
    for (auto tableIt = aliases.constBegin(); tableIt != aliases.constEnd(); ++tableIt) {
        QJsonObject tableObj;
        for (auto entryIt = tableIt.value().constBegin();
             entryIt != tableIt.value().constEnd(); ++entryIt) {
            tableObj[entryIt.key()] = entryIt.value();
        }
        aliasRoot[tableIt.key()] = tableObj;
    }
    root[QStringLiteral("aliases")] = aliasRoot;

    QJsonArray rulesArr;
    for (const CommandRule &rule : rules)
        rulesArr.append(rule.toJson());
    root[QStringLiteral("rules")] = rulesArr;

    return root;
}

CommandRuleSet CommandRuleSet::fromJson(const QJsonObject &obj, QString *error)
{
    CommandRuleSet set;
    set.schemaVersion = obj.value(QStringLiteral("schema_version")).toInt(1);

    const QJsonObject aliasRoot = obj.value(QStringLiteral("aliases")).toObject();
    for (auto tableIt = aliasRoot.constBegin(); tableIt != aliasRoot.constEnd(); ++tableIt) {
        QMap<QString, QString> table;
        const QJsonObject tableObj = tableIt.value().toObject();
        for (auto entryIt = tableObj.constBegin(); entryIt != tableObj.constEnd(); ++entryIt)
            table[entryIt.key()] = entryIt.value().toString();
        set.aliases[tableIt.key()] = table;
        set.aliasTables.append(tableIt.key());
    }

    const QJsonArray rulesArr = obj.value(QStringLiteral("rules")).toArray();
    for (const QJsonValue &v : rulesArr) {
        QString ruleError;
        CommandRule rule = CommandRule::fromJson(v.toObject(), &ruleError);
        if (!ruleError.isEmpty() && error && error->isEmpty())
            *error = ruleError;
        set.rules.append(rule);
    }

    return set;
}

} // namespace dc
