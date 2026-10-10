// ============================================================================
//  ParseResult.h — 命令解析结果
//
//  需求依据：SRS V1.1 第 3.4 节
//    class CommandEngine:
//        def parse(self, text, language="auto"):
//            """解析输入，返回意图、参数、置信度和澄清需求。"""
//
//  本文件实现该返回值的 C++ 表达。
// ============================================================================
#pragma once

#include "model/CommandRule.h"
#include "model/Types.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 解析结论
// ---------------------------------------------------------------------------
enum class ParseOutcome {
    Matched = 0,        // 唯一匹配到一条规则
    Ambiguous,          // 匹配到多条同优先级规则，需要用户澄清（SRS 4.6）
    NoMatch,            // 完全没有匹配
    LowConfidence,      // 有匹配但置信度低于阈值
    Invalid             // 输入非法（空、超长、含控制字符）
};

QString parseOutcomeToString(ParseOutcome outcome);

// ---------------------------------------------------------------------------
// 一个候选匹配
// ---------------------------------------------------------------------------
struct CommandCandidate {
    QString ruleId;
    QString intent;
    QString matchedPattern;          // 实际命中的模式串
    QMap<QString, QString> parameters;  // 已提取并校验通过的参数
    double confidence = 0.0;         // 0.0 ~ 1.0
    RiskLevel riskLevel = RiskLevel::Low;
    int priority = 0;
    QString description;
};

// ---------------------------------------------------------------------------
// 解析结果
// ---------------------------------------------------------------------------
struct ParseResult {
    ParseOutcome outcome = ParseOutcome::NoMatch;
    Language detectedLanguage = Language::Auto;
    QString normalizedInput;         // 归一化（去空白、全角转半角、大小写折叠）后的输入
    CommandCandidate best;           // outcome == Matched / LowConfidence 时有效
    QVector<CommandCandidate> candidates;  // outcome == Ambiguous 时用于展示澄清选项
    QString clarificationPrompt;     // 面向用户的澄清问句（SRS 4.2 TXT-004）
    QString errorMessage;

    bool isExecutable() const
    {
        return outcome == ParseOutcome::Matched && best.confidence >= 1.0 - 1e-9;
    }

    QJsonObject toJson() const;
};

} // namespace dc
