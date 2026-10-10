// ============================================================================
//  PermissionManager.cpp — 权限策略实现
//
//  需求依据：SRS V1.1 4.4（8 条要求）、7.3、9.2
//
//  本文件是整个项目安全性最关键的一处。实现原则：
//    「任何不确定的情况一律取最严格处理，且必须给出可解释的理由。」
// ============================================================================
#include "engine/PermissionManager.h"

#include "core/BuildConfig.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"
#include "platform/PlatformAdapter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace dc {

// ===========================================================================
//  内部辅助：从 PermissionRequest 推导「实际生效风险等级」
//
//  放在文件顶部（匿名命名空间）而不是类私有方法，是为了让所有审计写入点
//  都能复用同一次计算，保证「判定时的风险」与「记录里的风险」永远一致。
// ===========================================================================
namespace {

RiskLevel effectiveRiskOf(const PermissionRequest &req)
{
    return PermissionManager::escalateRisk(
        req.riskLevel, req.sensitivity, req.batchOperation, req.irreversible);
}

} // namespace

// ===========================================================================
//  GrantEntry 序列化
// ===========================================================================
QJsonObject GrantEntry::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("rule_id")] = ruleId;
    o[QStringLiteral("target_fingerprint")] = targetFingerprint;
    o[QStringLiteral("scope")] = grantScopeToString(scope);
    o[QStringLiteral("granted_at")] = grantedAt.toString(Qt::ISODate);
    if (expiresAt.isValid())
        o[QStringLiteral("expires_at")] = expiresAt.toString(Qt::ISODate);
    return o;
}

GrantEntry GrantEntry::fromJson(const QJsonObject &obj)
{
    GrantEntry e;
    e.ruleId = obj.value(QStringLiteral("rule_id")).toString();
    e.targetFingerprint = obj.value(QStringLiteral("target_fingerprint")).toString();

    bool ok = false;
    e.scope = grantScopeFromString(obj.value(QStringLiteral("scope")).toString(), &ok);
    if (!ok)
        e.scope = GrantScope::ThisActionOnly;   // 解析失败取最小授权

    e.grantedAt = QDateTime::fromString(
        obj.value(QStringLiteral("granted_at")).toString(), Qt::ISODate);
    e.expiresAt = QDateTime::fromString(
        obj.value(QStringLiteral("expires_at")).toString(), Qt::ISODate);
    return e;
}

// ===========================================================================
//  构造
// ===========================================================================
PermissionManager::PermissionManager(PlatformAdapter *adapter, QObject *parent)
    : QObject(parent)
    , m_adapter(adapter)
{
}

PermissionManager::~PermissionManager()
{
    // 退出前把永久授权与审计落盘
    if (!m_auditPath.isEmpty())
        saveAudit();
}

void PermissionManager::setPolicy(const PermissionPolicy &policy)
{
    m_policy = policy;
}

PermissionPolicy PermissionManager::policy() const
{
    return m_policy;
}

void PermissionManager::setAuditStoragePath(const QString &path)
{
    m_auditPath = path;
    loadAudit();
}

QString PermissionManager::auditStoragePath() const
{
    return m_auditPath;
}

// ===========================================================================
//  禁止类意图
//
//  SRS 4.4 表格「禁止」行：绕过系统安全机制、隐蔽执行、未经同意的持续监控。
//  本程序不提供这些能力。这里用关键词兜底，防止规则文件被篡改后
//  出现这类意图 —— 这是「配置可以被编辑」带来的必要防线。
// ===========================================================================
QStringList PermissionManager::forbiddenIntentKeywords()
{
    return QStringList{
        // 绕过安全机制
        QStringLiteral("bypass"), QStringLiteral("disable_defender"),
        QStringLiteral("disable_uac"), QStringLiteral("disable_firewall"),
        QStringLiteral("disable_antivirus"), QStringLiteral("disable_selinux"),
        QStringLiteral("disable_apparmor"),
        QStringLiteral("绕过"), QStringLiteral("关闭防护"), QStringLiteral("关闭杀毒"),
        QStringLiteral("关闭防火墙"), QStringLiteral("关闭安全"),

        // 隐蔽执行
        QStringLiteral("hidden_exec"), QStringLiteral("silent_exec"),
        QStringLiteral("no_window_exec"), QStringLiteral("covert"),
        QStringLiteral("隐蔽执行"), QStringLiteral("静默执行"), QStringLiteral("后台隐蔽"),

        // 持续监控 / 窃取
        QStringLiteral("keylogger"), QStringLiteral("screen_spy"),
        QStringLiteral("continuous_monitor"), QStringLiteral("surveillance"),
        QStringLiteral("键盘记录"), QStringLiteral("屏幕监控"), QStringLiteral("持续监控"),
        QStringLiteral("窃取"), QStringLiteral("steal"), QStringLiteral("exfiltrate"),

        // 权限提升滥用
        QStringLiteral("privilege_escalation"), QStringLiteral("run_as_system"),
        QStringLiteral("提权"), QStringLiteral("获取管理员"),

        // 注入
        QStringLiteral("inject"), QStringLiteral("注入"), QStringLiteral("hook_process"),

        // 持久化后门
        QStringLiteral("backdoor"), QStringLiteral("后门"),
        QStringLiteral("persist_service"), QStringLiteral("安装服务后门"),
    };
}

bool PermissionManager::isForbiddenIntent(const QString &intent)
{
    if (intent.trimmed().isEmpty())
        return false;

    const QString lowered = intent.toLower();

    const QStringList keywords = forbiddenIntentKeywords();
    for (const QString &kw : keywords) {
        if (lowered.contains(kw.toLower()))
            return true;
    }

    return false;
}

// ===========================================================================
//  风险升级
//
//  规则文件里声明的 risk_level 是「基线」，实际风险由目标特征决定：
//    - 目标落在敏感位置（启动项、系统目录）-> 至少 medium，critical 位置 -> high
//    - 批量操作                          -> 至少 medium（SRS 4.3）
//    - 不可逆操作                        -> 至少 medium；若同时敏感 -> high
//
//  只升不降：规则作者不能通过低声明来规避敏感位置带来的更强确认。
// ===========================================================================
RiskLevel PermissionManager::escalateRisk(RiskLevel baseline,
                                        SensitivityLevel sensitivity,
                                        bool batchOperation,
                                        bool irreversible)
{
    // 禁止级不可被降低
    if (baseline == RiskLevel::Forbidden)
        return RiskLevel::Forbidden;

    RiskLevel level = baseline;

    auto raiseTo = [&level](RiskLevel target) {
        if (static_cast<int>(target) > static_cast<int>(level))
            level = target;
    };

    switch (sensitivity) {
    case SensitivityLevel::Normal:
        break;
    case SensitivityLevel::Sensitive:
        // 启动项、用户配置、程序目录：至少需要确认
        raiseTo(RiskLevel::Medium);
        break;
    case SensitivityLevel::Critical:
        // 系统目录、注册表、账户、网络安全配置：必须明确授权
        // SRS 4.4 第 6 条：「对系统目录、启动项、账户、网络安全配置等
        //                  敏感位置实行更严格确认。」
        raiseTo(RiskLevel::High);
        break;
    }

    if (batchOperation)
        raiseTo(RiskLevel::Medium);

    if (irreversible) {
        // 不可逆操作本身就需要确认；若已是高危则保持高危
        raiseTo(RiskLevel::Medium);
        if (sensitivity != SensitivityLevel::Normal)
            raiseTo(RiskLevel::High);
    }

    return level;
}

// ===========================================================================
//  目标指纹
//
//  多目标时把所有规范化路径排序后串联再哈希，保证：
//    - 同一组目标 -> 同一指纹（授权可复用）
//    - 目标集合变化 -> 指纹变化（授权失效，符合 SRS 4.4 第 5 条）
// ===========================================================================
QString PermissionManager::computeTargetFingerprint(const PermissionRequest &request)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);

    hash.addData(request.ruleId.toUtf8());
    hash.addData("|");
    hash.addData(request.intent.toUtf8());
    hash.addData("|");

    // 路径：规范化 + 排序，消除顺序差异带来的指纹不稳定
    QStringList paths = request.affectedPaths;
    for (QString &p : paths)
        p = PathGuard::normalize(p);
    paths.sort();

    for (const QString &p : paths) {
        hash.addData(p.toUtf8());
        hash.addData("\n");
    }

    hash.addData("|");

    QStringList targets = request.affectedTargets;
    targets.sort();
    for (const QString &t : targets) {
        hash.addData(t.toUtf8());
        hash.addData("\n");
    }

    return QString::fromLatin1(hash.result().toHex().left(32));
}

// ===========================================================================
//  授权查询
// ===========================================================================
bool PermissionManager::hasGrant(const QString &ruleId, const QString &fingerprint,
                                GrantScope *scope) const
{
    const QString key = ruleId + QLatin1Char('|') + fingerprint;

    if (!m_grants.contains(key))
        return false;

    const GrantEntry &entry = m_grants.value(key);
    if (!entry.isActive())
        return false;

    if (scope)
        *scope = entry.scope;

    return true;
}

bool PermissionManager::isDenied(const QString &ruleId, const QString &fingerprint,
                                QString *reason) const
{
    const QString key = ruleId + QLatin1Char('|') + fingerprint;

    if (!m_denials.contains(key))
        return false;

    const DenialRecord &rec = m_denials.value(key);
    if (!rec.isActive())
        return false;

    if (reason) {
        *reason = QStringLiteral(
            "你此前已拒绝过这一操作（%1），本次不再执行。"
            "如需改变，请在设置中清除拒绝记录，或等待拒绝记录过期（%2 分钟后）。")
            .arg(rec.deniedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
            .arg(rec.deniedAt.secsTo(rec.expiresAt) / 60);
    }

    return true;
}

// ===========================================================================
//  核心：权限检查
// ===========================================================================
PermissionDecision PermissionManager::check(const PermissionRequest &request) const
{
    PermissionDecision decision;

    // 计算实际风险等级（只升不降）
    const RiskLevel effectiveRisk = escalateRisk(
        request.riskLevel, request.sensitivity,
        request.batchOperation, request.irreversible);

    decision.riskLevel = effectiveRisk;
    decision.sensitivity = request.sensitivity;

    // -----------------------------------------------------------------------
    // 第 1 关：禁止类意图（SRS 4.4：不提供）
    //   注意：这一关在「拒绝记录」之前判定，因为禁止类操作无论如何都不提供，
    //         不因用户是否曾拒绝而改变。
    // -----------------------------------------------------------------------
    if (effectiveRisk == RiskLevel::Forbidden || isForbiddenIntent(request.intent)) {
        decision.verdict = PermissionVerdict::Denied;
        decision.reason = QStringLiteral(
            "出于安全考虑，本程序不提供此类操作（%1）。"
            "这包括绕过系统安全机制、隐蔽执行和未经同意的持续监控。")
            .arg(request.intent);

        DC_WARN(LogCat::Permission,
                QStringLiteral("拒绝禁止级操作：%1").arg(request.toSafeLogString()));
        return decision;
    }

    // -----------------------------------------------------------------------
    // 第 2 关：拒绝锁定期（SRS 4.4 第 8 条）
    // -----------------------------------------------------------------------
    const QString fingerprint = computeTargetFingerprint(request);

    QString denialReason;
    if (isDenied(request.ruleId, fingerprint, &denialReason)) {
        decision.verdict = PermissionVerdict::Denied;
        decision.reason = denialReason;

        DC_INFO(LogCat::Permission,
                QStringLiteral("命中拒绝记录，直接拒绝：%1")
                    .arg(request.toSafeLogString()));
        return decision;
    }

    // -----------------------------------------------------------------------
    // 第 3 关：已有授权（SRS 4.4 第 4、5 条）
    //   只有「规则 + 目标指纹」完全一致才算命中，因此不会扩大解释。
    // -----------------------------------------------------------------------
    GrantScope matchedScope = GrantScope::ThisActionOnly;
    if (hasGrant(request.ruleId, fingerprint, &matchedScope)) {
        decision.verdict = PermissionVerdict::Allowed;
        decision.matchedGrant = true;
        decision.matchedScope = matchedScope;
        decision.reason = matchedScope == GrantScope::RulePermanent
            ? QStringLiteral("你此前已长期允许该操作，本次直接执行。")
            : QStringLiteral("你在本会话中已允许该操作，本次直接执行。");

        // 会话授权：检查是否需要按次数消费（本项目采用「会话内有效」语义，
        // 因此不消费，到期后自然失效）
        return decision;
    }

    // -----------------------------------------------------------------------
    // 第 4 关：按风险等级决定
    // -----------------------------------------------------------------------
    switch (effectiveRisk) {
    case RiskLevel::Low:
        if (m_policy.confirmLowRisk || request.requiresConfirmation()) {
            decision.verdict = PermissionVerdict::NeedConfirm;
            decision.reason = QStringLiteral("该操作需要确认后执行。");
        } else {
            decision.verdict = PermissionVerdict::Allowed;
            decision.reason = QStringLiteral("低风险操作，按配置直接执行。");
        }
        break;

    case RiskLevel::Medium:
        if (m_policy.confirmMediumRisk) {
            decision.verdict = PermissionVerdict::NeedConfirm;
            decision.reason = QStringLiteral(
                "该操作会修改数据，需要你确认后再执行。");
        } else {
            decision.verdict = PermissionVerdict::Allowed;
            decision.reason = QStringLiteral("中等风险操作，按配置直接执行。");
        }
        break;

    case RiskLevel::High:
        if (m_policy.authorizeHighRisk) {
            decision.verdict = PermissionVerdict::NeedAuthorize;
            decision.reason = request.elevationRequired
                ? QStringLiteral(
                      "该操作需要管理员权限，且会影响系统设置，"
                      "必须由你明确授权后，由系统提示完成权限提升。")
                : QStringLiteral(
                      "该操作风险较高（%1），必须由你明确授权后才能执行。")
                      .arg(request.targetDescription.isEmpty()
                               ? QStringLiteral("可能影响系统状态")
                               : request.targetDescription);
        } else {
            decision.verdict = PermissionVerdict::Allowed;
            decision.reason = QStringLiteral("高风险操作，按配置直接执行（策略已放宽）。");
        }
        break;

    case RiskLevel::Forbidden:
        // 前面已处理，这里做防御性兜底
        decision.verdict = PermissionVerdict::Denied;
        decision.reason = QStringLiteral("该操作不受支持。");
        break;
    }

    DC_DEBUG(LogCat::Permission,
             QStringLiteral("权限判定：%1 -> %2（%3）")
                 .arg(request.toSafeLogString(),
                      permissionVerdictToString(decision.verdict),
                      decision.reason.left(60)));

    return decision;
}

// ===========================================================================
//  用户决策登记
// ===========================================================================
void PermissionManager::recordUserDecision(const PermissionRequest &request,
                                          bool granted,
                                          GrantScope scope)
{
    const QString fingerprint = computeTargetFingerprint(request);
    const QString key = request.ruleId + QLatin1Char('|') + fingerprint;

    const QDateTime now = QDateTime::currentDateTime();

    if (!granted) {
        // ---- 写入拒绝记录（SRS 4.4 第 8 条）----
        DenialRecord rec;
        rec.ruleId = request.ruleId;
        rec.targetFingerprint = fingerprint;
        rec.deniedAt = now;
        rec.expiresAt = now.addSecs(m_policy.denialRetentionMinutes * 60);
        rec.reason = QStringLiteral("用户拒绝授权");

        m_denials.insert(key, rec);

        appendAudit(AuditRecord{
            now, request.ruleId, request.intent,
            effectiveRiskOf(request), request.sensitivity,
            QStringLiteral("denied"), QStringLiteral("cancelled"),
            request.targetDescription,
            QStringLiteral("用户在授权对话框中选择了拒绝"),
        });

        DC_INFO(LogCat::Permission,
                QStringLiteral("用户拒绝授权，已记录：%1").arg(request.toSafeLogString()));

        emit grantsChanged();
        return;
    }

    // ---- 写入授权记录 ----
    if (scope == GrantScope::ThisActionOnly) {
        // 一次性授权：本次直接执行，不进入长期表。
        // 上层在执行时不再调用 check()，因此无需记录。
        appendAudit(AuditRecord{
            now, request.ruleId, request.intent,
            effectiveRiskOf(request), request.sensitivity,
            QStringLiteral("granted_once"), QStringLiteral("succeeded"),
            request.targetDescription,
            QStringLiteral("用户同意本次执行"),
        });

        // 顺手清理可能存在的旧拒绝记录（用户现在愿意了）
        m_denials.remove(key);

        emit grantsChanged();
        return;
    }

    // 会话级 / 永久级授权
    GrantEntry entry;
    entry.ruleId = request.ruleId;
    entry.targetFingerprint = fingerprint;
    entry.scope = scope;
    entry.grantedAt = now;

    if (scope == GrantScope::RuleForSession) {
        entry.expiresAt = now.addSecs(m_policy.sessionGrantLifetimeMinutes * 60);
    } else {
        // 永久授权：检查数量上限（防滥用）
        if (!m_policy.allowPermanentGrants) {
            DC_WARN(LogCat::Permission,
                    QStringLiteral("用户请求永久授权，但策略不允许，已降级为会话授权。"));
            entry.scope = GrantScope::RuleForSession;
            entry.expiresAt = now.addSecs(m_policy.sessionGrantLifetimeMinutes * 60);
        } else {
            int permanentCount = 0;
            for (auto it = m_grants.constBegin(); it != m_grants.constEnd(); ++it) {
                if (it.value().ruleId == request.ruleId
                    && it.value().scope == GrantScope::RulePermanent
                    && it.value().isActive()) {
                    ++permanentCount;
                }
            }

            if (permanentCount >= m_policy.maxPermanentGrantsPerRule) {
                DC_WARN(LogCat::Permission,
                        QStringLiteral("规则 %1 的永久授权数已达上限，降级为会话授权。")
                            .arg(request.ruleId));
                entry.scope = GrantScope::RuleForSession;
                entry.expiresAt = now.addSecs(m_policy.sessionGrantLifetimeMinutes * 60);
            }
            // 永久授权 expiresAt 保持无效（不过期）
        }
    }

    m_grants.insert(key, entry);
    m_denials.remove(key);

    appendAudit(AuditRecord{
        now, request.ruleId, request.intent,
        effectiveRiskOf(request), request.sensitivity,
        scope == GrantScope::RulePermanent
            ? QStringLiteral("granted_permanent")
            : QStringLiteral("granted_session"),
        QStringLiteral("succeeded"),
        request.targetDescription,
        QStringLiteral("授权范围：%1").arg(grantScopeToString(scope)),
    });

    DC_INFO(LogCat::Permission,
            QStringLiteral("用户授权：%1，范围=%2")
                .arg(request.toSafeLogString(), grantScopeToString(scope)));

    emit grantsChanged();
}

// ===========================================================================
//  执行结果登记
// ===========================================================================
void PermissionManager::recordOutcome(const PermissionRequest &request,
                                     const QString &result,
                                     const QString &detail,
                                     const QString &authorizationState)
{
    AuditRecord rec;
    rec.timestamp = QDateTime::currentDateTime();
    rec.ruleId = request.ruleId;
    rec.intent = request.intent;
    rec.riskLevel = effectiveRiskOf(request);
    rec.sensitivity = request.sensitivity;
    rec.authorizationState = authorizationState.isEmpty()
        ? QStringLiteral("not_required")
        : authorizationState;
    rec.result = result;
    rec.targetSummary = request.targetDescription;
    rec.detail = detail;

    appendAudit(rec);
}

// ===========================================================================
//  授权管理
// ===========================================================================
QVector<GrantEntry> PermissionManager::activeGrants() const
{
    QVector<GrantEntry> out;
    const QDateTime now = QDateTime::currentDateTime();

    for (auto it = m_grants.constBegin(); it != m_grants.constEnd(); ++it) {
        if (it.value().isActive(now))
            out.append(it.value());
    }

    return out;
}

void PermissionManager::revokeGrant(const QString &ruleId,
                                   const QString &targetFingerprint)
{
    const QString key = ruleId + QLatin1Char('|') + targetFingerprint;
    if (m_grants.remove(key) > 0) {
        DC_INFO(LogCat::Permission,
                QStringLiteral("已撤销授权：规则=%1 指纹=%2")
                    .arg(ruleId, targetFingerprint.left(8)));

        appendAudit(AuditRecord{
            QDateTime::currentDateTime(), ruleId, QString(),
            RiskLevel::Low, SensitivityLevel::Normal,
            QStringLiteral("revoked"), QStringLiteral("succeeded"),
            targetFingerprint.left(8),
            QStringLiteral("用户在设置中撤销了该授权"),
        });

        emit grantsChanged();
    }
}

void PermissionManager::revokeAllGrantsForRule(const QString &ruleId)
{
    QList<QString> keys;
    for (auto it = m_grants.constBegin(); it != m_grants.constEnd(); ++it) {
        if (it.value().ruleId == ruleId)
            keys.append(it.key());
    }

    for (const QString &key : keys)
        m_grants.remove(key);

    if (!keys.isEmpty()) {
        DC_INFO(LogCat::Permission,
                QStringLiteral("已撤销规则 %1 的全部授权（%2 条）")
                    .arg(ruleId).arg(keys.size()));
        emit grantsChanged();
    }
}

void PermissionManager::revokeAllGrants()
{
    if (m_grants.isEmpty())
        return;

    const int count = m_grants.size();
    m_grants.clear();

    DC_INFO(LogCat::Permission,
            QStringLiteral("已撤销全部授权（%1 条）").arg(count));
    emit grantsChanged();
}

QVector<DenialRecord> PermissionManager::activeDenials() const
{
    QVector<DenialRecord> out;
    const QDateTime now = QDateTime::currentDateTime();

    for (auto it = m_denials.constBegin(); it != m_denials.constEnd(); ++it) {
        if (it.value().isActive(now))
            out.append(it.value());
    }

    return out;
}

void PermissionManager::clearDenial(const QString &ruleId,
                                   const QString &targetFingerprint)
{
    const QString key = ruleId + QLatin1Char('|') + targetFingerprint;
    if (m_denials.remove(key) > 0) {
        DC_INFO(LogCat::Permission,
                QStringLiteral("已清除拒绝记录：规则=%1").arg(ruleId));
        emit grantsChanged();
    }
}

void PermissionManager::clearAllDenials()
{
    if (m_denials.isEmpty())
        return;

    const int count = m_denials.size();
    m_denials.clear();

    DC_INFO(LogCat::Permission,
            QStringLiteral("已清除全部拒绝记录（%1 条）").arg(count));
    emit grantsChanged();
}

// ===========================================================================
//  审计
// ===========================================================================
QVector<AuditRecord> PermissionManager::recentAudit(int limit) const
{
    if (limit <= 0 || limit >= m_audit.size())
        return m_audit;

    return m_audit.mid(m_audit.size() - limit);
}

int PermissionManager::auditCount() const
{
    return m_audit.size();
}

void PermissionManager::appendAudit(const AuditRecord &record)
{
    m_audit.append(record);

    // 内存上限：超出后丢弃最老的记录。
    // 注意：落盘文件不受此限制，用户可通过导出获取完整记录。
    while (m_audit.size() > kMaxInMemoryAudit)
        m_audit.removeFirst();
}

Result PermissionManager::exportAudit(const QString &path) const
{
    QJsonArray arr;
    for (const AuditRecord &rec : m_audit)
        arr.append(rec.toJson());

    QJsonObject root;
    root[QStringLiteral("exported_at")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("app_version")] = QString::fromLatin1(dc::buildinfo::kAppVersion);
    root[QStringLiteral("record_count")] = m_audit.size();
    root[QStringLiteral("records")] = arr;

    // 导出内容说明（SRS 7.3：说明记录了什么，避免用户误解）
    root[QStringLiteral("note")] = QStringLiteral(
        "本文件记录重要操作的类型、时间、结果与授权状态。"
        "为避免泄露敏感信息，目标以摘要或指纹形式记录，不含完整路径。");

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Result::failure(erc::HistoryExportFailed,
                               QStringLiteral("无法写入审计记录文件：%1")
                                   .arg(file.errorString()));
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();

    return Result::success();
}

void PermissionManager::clearAudit()
{
    m_audit.clear();
    if (!m_auditPath.isEmpty())
        saveAudit();
}

void PermissionManager::loadAudit()
{
    if (m_auditPath.isEmpty() || !QFile::exists(m_auditPath))
        return;

    QFile file(m_auditPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject())
        return;

    const QJsonArray arr = doc.object().value(QStringLiteral("records")).toArray();
    m_audit.clear();

    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        AuditRecord rec;
        rec.timestamp = QDateTime::fromString(
            o.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
        rec.ruleId = o.value(QStringLiteral("rule_id")).toString();
        rec.intent = o.value(QStringLiteral("intent")).toString();
        rec.authorizationState = o.value(QStringLiteral("authorization_state")).toString();
        rec.result = o.value(QStringLiteral("result")).toString();
        rec.targetSummary = o.value(QStringLiteral("target_summary")).toString();
        rec.detail = o.value(QStringLiteral("detail")).toString();
        m_audit.append(rec);
    }

    DC_INFO(LogCat::Permission,
            QStringLiteral("已加载 %1 条审计记录").arg(m_audit.size()));
}

void PermissionManager::saveAudit() const
{
    if (m_auditPath.isEmpty())
        return;

    QDir().mkpath(QFileInfo(m_auditPath).absolutePath());

    QJsonArray arr;
    // 只落盘最近 5000 条，避免文件无限增长
    const int start = qMax(0, m_audit.size() - 5000);
    for (int i = start; i < m_audit.size(); ++i)
        arr.append(m_audit.at(i).toJson());

    QJsonObject root;
    root[QStringLiteral("saved_at")] =
        QDateTime::currentDateTime().toString(Qt::ISODate);
    root[QStringLiteral("records")] = arr;

    QFile file(m_auditPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
}

// ===========================================================================
//  过期清理
// ===========================================================================
void PermissionManager::pruneExpired()
{
    const QDateTime now = QDateTime::currentDateTime();

    QList<QString> grantKeys;
    for (auto it = m_grants.constBegin(); it != m_grants.constEnd(); ++it) {
        if (!it.value().isActive(now))
            grantKeys.append(it.key());
    }
    for (const QString &k : grantKeys)
        m_grants.remove(k);

    QList<QString> denialKeys;
    for (auto it = m_denials.constBegin(); it != m_denials.constEnd(); ++it) {
        if (!it.value().isActive(now))
            denialKeys.append(it.key());
    }
    for (const QString &k : denialKeys)
        m_denials.remove(k);

    if (!grantKeys.isEmpty() || !denialKeys.isEmpty()) {
        DC_DEBUG(LogCat::Permission,
                 QStringLiteral("已清理过期授权 %1 条、过期拒绝记录 %2 条")
                     .arg(grantKeys.size()).arg(denialKeys.size()));
    }
}

} // namespace dc
