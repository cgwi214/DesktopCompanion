// ============================================================================
//  ParseResult.cpp
// ============================================================================
#include "model/ParseResult.h"

#include <QJsonArray>

namespace dc {

QString parseOutcomeToString(ParseOutcome outcome)
{
    switch (outcome) {
    case ParseOutcome::Matched:       return QStringLiteral("matched");
    case ParseOutcome::Ambiguous:     return QStringLiteral("ambiguous");
    case ParseOutcome::NoMatch:       return QStringLiteral("no_match");
    case ParseOutcome::LowConfidence: return QStringLiteral("low_confidence");
    case ParseOutcome::Invalid:       return QStringLiteral("invalid");
    }
    return QStringLiteral("no_match");
}

QJsonObject ParseResult::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("outcome")] = parseOutcomeToString(outcome);
    obj[QStringLiteral("detected_language")] = languageToCode(detectedLanguage);
    obj[QStringLiteral("normalized_input")] = normalizedInput;
    obj[QStringLiteral("clarification_prompt")] = clarificationPrompt;

    if (!best.ruleId.isEmpty()) {
        QJsonObject bestObj;
        bestObj[QStringLiteral("rule_id")] = best.ruleId;
        bestObj[QStringLiteral("intent")] = best.intent;
        bestObj[QStringLiteral("matched_pattern")] = best.matchedPattern;
        bestObj[QStringLiteral("confidence")] = best.confidence;
        bestObj[QStringLiteral("risk_level")] = riskLevelToString(best.riskLevel);
        QJsonObject params;
        for (auto it = best.parameters.constBegin(); it != best.parameters.constEnd(); ++it)
            params[it.key()] = it.value();
        bestObj[QStringLiteral("parameters")] = params;
        obj[QStringLiteral("best")] = bestObj;
    }

    if (!candidates.isEmpty()) {
        QJsonArray arr;
        for (const CommandCandidate &c : candidates) {
            QJsonObject cObj;
            cObj[QStringLiteral("rule_id")] = c.ruleId;
            cObj[QStringLiteral("intent")] = c.intent;
            cObj[QStringLiteral("description")] = c.description;
            cObj[QStringLiteral("confidence")] = c.confidence;
            arr.append(cObj);
        }
        obj[QStringLiteral("candidates")] = arr;
    }

    if (!errorMessage.isEmpty())
        obj[QStringLiteral("error")] = errorMessage;

    return obj;
}

} // namespace dc
