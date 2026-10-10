// ============================================================================
//  CommandRule.h — 命令规则数据结构
//
//  需求依据：SRS V1.1 第 4.6 节（命令引擎）
//
//  SRS 原文给出的规则示例：
//    {
//      "id": "open_browser",
//      "language": "zh-CN",
//      "patterns": ["打开浏览器", "启动浏览器"],
//      "intent": "open_application",
//      "parameters": { "app_id": "browser" },
//      "risk_level": "low",
//      "confirmation": false
//    }
//
//  本文件是该 JSON 的 C++ 映射，并补充了「参数提取」所需的元信息。
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 参数模板
//   描述一条规则如何从用户输入中取出参数值。三种来源：
//     1) Constant  —— 规则自带固定值（如 app_id = "browser"）
//     2) Slot      —— 从模式串的占位符位置截取（如 "打开{file}中的{name}"）
//     3) AliasMap  —— 用别名表映射（如「浏览器」→ 应用 id "browser"）
//
//  为什么不直接用自由文本拼接参数？
//    SRS 4.3 明确要求「启动参数默认不允许由未经校验的自由文本直接拼接」，
//    因此参数必须给出显式来源，而不是原样透传用户输入。
// ---------------------------------------------------------------------------
enum class ParameterSource {
    Constant = 0,     // 固定值
    Slot,             // 从占位符捕获组取值
    AliasMap          // 通过别名表归一化
};

QString parameterSourceToString(ParameterSource source);
ParameterSource parameterSourceFromString(const QString &text, bool *ok = nullptr);

struct ParameterTemplate {
    QString name;                          // 参数名，如 "app_id"
    ParameterSource source = ParameterSource::Constant;
    QString value;                         // Constant 时使用：固定值 / 别名表名
    QString slotName;                      // Slot 时使用：占位符名称
    // 取值必须匹配的校验正则（为空表示不额外校验）
    // SRS 7.3：所有外部输入均需校验
    QString validationPattern;
    bool required = true;

    QJsonObject toJson() const;
    static ParameterTemplate fromJson(const QJsonObject &obj);
};

// ---------------------------------------------------------------------------
// 单条命令规则
// ---------------------------------------------------------------------------
struct CommandRule {
    QString id;                     // 唯一标识，如 "open_browser"
    QString intent;                 // 意图名，如 "open_application"
    Language language = Language::Auto;  // 该规则所属语言；Auto 表示与语言无关
    QStringList patterns;           // 模式串列表（可含 {占位符}）
    QVector<ParameterTemplate> parameters;
    RiskLevel riskLevel = RiskLevel::Low;
    bool requiresConfirmation = false;   // 即使低风险也可要求确认
    int priority = 0;               // 优先级：数值越大越优先，用于消解冲突
    bool enabled = true;
    QString description;            // 面向用户/文档的说明
    QString category;               // 分组：pet / app / file / system / search ...

    // ---- 运行时派生数据（不参与序列化）----
    // 已编译的匹配正则；启动时由 CommandRepository 统一编译，
    // 避免每次匹配都重新编译（SRS 7.1：避免持续轮询与重复开销）
    //
    // 注：QRegularExpression 不可拷贝赋值给共享实例，这里使用值语义，
    //     由 Qt 内部隐式共享保证开销可接受。
    QVector<QRegularExpression> compiledPatterns;
    QVector<QString> patternSlotOrders;   // 每个模式对应的占位符顺序

    bool isValid() const;
    QString validationError() const;

    QJsonObject toJson() const;
    static CommandRule fromJson(const QJsonObject &obj, QString *error = nullptr);
};

// ---------------------------------------------------------------------------
// 规则集合：配置文件中的整体结构
// ---------------------------------------------------------------------------
struct CommandRuleSet {
    int schemaVersion = 1;
    QStringList aliasTables;                        // 声明的别名表名
    QMap<QString, QMap<QString, QString>> aliases;  // 表名 -> (别名 -> 归一化值)
    QVector<CommandRule> rules;

    QJsonObject toJson() const;
    static CommandRuleSet fromJson(const QJsonObject &obj, QString *error = nullptr);
};

} // namespace dc
