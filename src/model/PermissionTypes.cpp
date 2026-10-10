// ============================================================================
//  PermissionTypes.cpp
//
//  需求依据：SRS V1.1 第 4.4、7.3 节
// ============================================================================
#include "model/PermissionTypes.h"

#include <QCryptographicHash>

namespace dc {

QString sensitivityLevelToString(SensitivityLevel level)
{
    switch (level) {
    case SensitivityLevel::Normal:   return QStringLiteral("normal");
    case SensitivityLevel::Sensitive: return QStringLiteral("sensitive");
    case SensitivityLevel::Critical: return QStringLiteral("critical");
    }
    return QStringLiteral("critical");   // 未知按最严格处理
}

QString grantScopeToString(GrantScope scope)
{
    switch (scope) {
    case GrantScope::ThisActionOnly: return QStringLiteral("once");
    case GrantScope::RuleForSession: return QStringLiteral("session");
    case GrantScope::RulePermanent:  return QStringLiteral("permanent");
    }
    return QStringLiteral("once");
}

GrantScope grantScopeFromString(const QString &text, bool *ok)
{
    const QString key = text.trimmed().toLower();
    if (key == QStringLiteral("once") || key == QStringLiteral("this_action_only")) {
        if (ok) *ok = true;
        return GrantScope::ThisActionOnly;
    }
    if (key == QStringLiteral("session") || key == QStringLiteral("rule_for_session")) {
        if (ok) *ok = true;
        return GrantScope::RuleForSession;
    }
    if (key == QStringLiteral("permanent") || key == QStringLiteral("rule_permanent")) {
        if (ok) *ok = true;
        return GrantScope::RulePermanent;
    }
    if (ok) *ok = false;
    // 解析失败 -> 最小授权（SRS 4.4 第 5 条：不得扩大解释授权）
    return GrantScope::ThisActionOnly;
}

// ---------------------------------------------------------------------------
//  PermissionRequest
// ---------------------------------------------------------------------------
QJsonObject PermissionRequest::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("rule_id")] = ruleId;
    obj[QStringLiteral("intent")] = intent;
    obj[QStringLiteral("action_summary")] = actionSummary;
    obj[QStringLiteral("target_description")] = targetDescription;
    obj[QStringLiteral("impact_description")] = impactDescription;
    obj[QStringLiteral("required_privilege")] = requiredPrivilege;
    obj[QStringLiteral("risk_level")] = riskLevelToString(riskLevel);
    obj[QStringLiteral("sensitivity")] = sensitivityLevelToString(sensitivity);
    obj[QStringLiteral("irreversible")] = irreversible;
    obj[QStringLiteral("elevation_required")] = elevationRequired;
    obj[QStringLiteral("batch_operation")] = batchOperation;
    obj[QStringLiteral("rule_requires_confirmation")] = ruleRequiresConfirmation;
    obj[QStringLiteral("affected_path_count")] = affectedPaths.size();
    return obj;
}

QString PermissionRequest::toSafeLogString() const
{
    // SRS 7.3：避免记录不必要的敏感内容。
    // 这里只输出规则、风险等级、目标数量与目标指纹，不输出完整路径。
    QString fingerprint;
    if (!affectedPaths.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (const QString &p : affectedPaths)
            hash.addData(p.toUtf8());
        fingerprint = QString::fromLatin1(hash.result().toHex().left(12));
    }

    return QStringLiteral("rule=%1 risk=%2 sensitivity=%3 targets=%4 fingerprint=%5 irreversible=%6")
        .arg(ruleId,
             riskLevelToString(riskLevel),
             sensitivityLevelToString(sensitivity))
        .arg(affectedPaths.size())
        .arg(fingerprint.isEmpty() ? QStringLiteral("-") : fingerprint,
             irreversible ? QStringLiteral("yes") : QStringLiteral("no"));
}

// ---------------------------------------------------------------------------
//  DenialRecord
// ---------------------------------------------------------------------------
QJsonObject DenialRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("rule_id")] = ruleId;
    obj[QStringLiteral("target_fingerprint")] = targetFingerprint;
    obj[QStringLiteral("denied_at")] = deniedAt.toString(Qt::ISODate);
    obj[QStringLiteral("expires_at")] = expiresAt.toString(Qt::ISODate);
    obj[QStringLiteral("reason")] = reason;
    return obj;
}

DenialRecord DenialRecord::fromJson(const QJsonObject &obj)
{
    DenialRecord rec;
    rec.ruleId = obj.value(QStringLiteral("rule_id")).toString();
    rec.targetFingerprint = obj.value(QStringLiteral("target_fingerprint")).toString();
    rec.deniedAt = QDateTime::fromString(
        obj.value(QStringLiteral("denied_at")).toString(), Qt::ISODate);
    rec.expiresAt = QDateTime::fromString(
        obj.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
    rec.reason = obj.value(QStringLiteral("reason")).toString();
    return rec;
}

// ---------------------------------------------------------------------------
//  AuditRecord
// ---------------------------------------------------------------------------
QJsonObject AuditRecord::toJson() const
{
    QJsonObject obj;
    obj[QStringLiteral("timestamp")] = timestamp.toString(Qt::ISODate);
    obj[QStringLiteral("rule_id")] = ruleId;
    obj[QStringLiteral("intent")] = intent;
    obj[QStringLiteral("risk_level")] = riskLevelToString(riskLevel);
    obj[QStringLiteral("sensitivity")] = sensitivityLevelToString(sensitivity);
    obj[QStringLiteral("authorization_state")] = authorizationState;
    obj[QStringLiteral("result")] = result;
    obj[QStringLiteral("target_summary")] = targetSummary;
    if (!detail.isEmpty())
        obj[QStringLiteral("detail")] = detail;
    return obj;
}

} // namespace dc
