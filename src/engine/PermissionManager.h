// ============================================================================
//  PermissionManager.h — 权限策略、风险分级与授权记录
//
//  需求依据：SRS V1.1 第 4.4 节（系统操作与权限）
//
//  风险等级与默认策略（SRS 表格）：
//    低   -> 打开应用、打开普通文档              可按配置直接执行
//    中   -> 修改用户级设置、批量处理用户文件    执行前确认
//    高   -> 管理员操作、系统目录写入、注册表修改 明确授权
//    禁止 -> 绕过安全机制、隐蔽执行、持续监控    不提供
//
//  SRS 4.4 的 8 条权限要求，逐条对应的实现机制：
//    1. 默认以普通用户权限运行      -> 程序启动时不做任何提权；elevationRequired
//                                      仅用于「如实告知用户需要管理员权限」
//    2. 不得暗中取得管理员权限      -> 本类不包含任何提权 API；
//                                      提权只能由 ShellExecute "runas" 触发，
//                                      且必须在用户明确授权后调用，并由系统
//                                      弹出 UAC —— 程序无法静默完成
//    3. 高权限操作展示目标/影响/权限 -> PermissionRequest 的四个描述字段，
//                                      由 buildRequest() 统一填充
//    4. 「记住授权」只针对具体操作类别 -> GrantScope 三档，默认仅本次
//    5. 不得扩大解释一次授权        -> 授权与 (ruleId, 目标指纹) 绑定，
//                                      目标变化即失效
//    6. 敏感位置更严格确认          -> SensitivityLevel 独立升级判定
//    7. 执行前验证、执行后记录      -> check() 前置 + recordOutcome() 后置审计
//    8. 拒绝后不得换路径重试        -> DenialRegistry，拒绝后同目标直接 Denied
// ============================================================================
#pragma once

#include "model/PermissionTypes.h"
#include "model/Types.h"

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QMap>
#include <QStringList>
#include <QVector>

namespace dc {

class PlatformAdapter;

// ---------------------------------------------------------------------------
// 授权记录（SRS 4.4 第 4、5 条）
//
//  关键设计：授权条目同时绑定「规则」与「目标指纹」。
//  这样「记住授权」的作用域是
//      「这条规则 + 这个具体目标」
//  而不是「这条规则」或「全局开关」，
//  从结构上杜绝了 SRS 4.4 第 5 条禁止的「扩大解释」。
// ---------------------------------------------------------------------------
struct GrantEntry {
    QString ruleId;
    QString targetFingerprint;
    GrantScope scope = GrantScope::ThisActionOnly;
    QDateTime grantedAt;
    QDateTime expiresAt;        // 无效表示不过期（仅 permanent 可不过期）

    bool isActive(const QDateTime &now = QDateTime::currentDateTime()) const
    {
        if (scope == GrantScope::ThisActionOnly)
            return false;   // 一次性授权在本次消费后即失效，不进入长期表
        if (expiresAt.isValid() && now >= expiresAt)
            return false;
        return true;
    }

    QJsonObject toJson() const;
    static GrantEntry fromJson(const QJsonObject &obj);
};

// ---------------------------------------------------------------------------
// 权限策略配置（可由用户调整，但有些底线不可调）
// ---------------------------------------------------------------------------
struct PermissionPolicy {
    // 低风险操作是否需要确认（默认 false，SRS 表格：可按配置直接执行）
    bool confirmLowRisk = false;

    // 中风险是否需要确认（SRS 表格：执行前确认）
    // 这是 SRS 明确要求的行为，因此不提供「关闭」选项，
    // 但保留字段以便未来引入「仅在批量操作时确认」这类细化策略。
    bool confirmMediumRisk = true;

    // 高风险是否需要明确授权（SRS 表格：明确授权）
    bool authorizeHighRisk = true;

    // 「记住授权」允许的最长时长（永久授权除外）
    int sessionGrantLifetimeMinutes = 480;   // 8 小时

    // 是否允许永久授权（用户可在设置中关闭，进一步收紧）
    bool allowPermanentGrants = true;

    // 单条规则最多可累计的永久授权数（防滥用）
    int maxPermanentGrantsPerRule = 2;

    // 拒绝记录的保持时长（SRS 4.4 第 8 条：拒绝后不得重试）
    // 到期后用户可再次被询问，避免永久锁死。
    int denialRetentionMinutes = 60;
};

// ---------------------------------------------------------------------------
// PermissionManager
// ---------------------------------------------------------------------------
class PermissionManager : public QObject
{
    Q_OBJECT

public:
    explicit PermissionManager(PlatformAdapter *adapter, QObject *parent = nullptr);
    ~PermissionManager() override;

    void setPolicy(const PermissionPolicy &policy);
    PermissionPolicy policy() const;

    // 绑定审计记录的持久化位置（可选；为空则只保留在内存最近 N 条）
    void setAuditStoragePath(const QString &path);
    QString auditStoragePath() const;

    // -----------------------------------------------------------------------
    // 核心：权限检查
    //
    //  SRS 3.4 契约：
    //    def check(self, action, context):
    //        """检查操作所需权限，返回允许、拒绝或需要确认。"""
    //
    //  返回 PermissionDecision：
    //    Allowed       -> 可直接执行
    //    NeedConfirm   -> 需要用户确认（中风险）
    //    NeedAuthorize -> 需要用户明确授权（高风险）
    //    Denied        -> 拒绝（禁止级，或此前已拒绝且未过期）
    // -----------------------------------------------------------------------
    PermissionDecision check(const PermissionRequest &request) const;

    // -----------------------------------------------------------------------
    // 授权决策登记（用户在授权对话框上做出的选择）
    //
    //  granted == false 时，会写入拒绝记录；
    //  之后同一 (ruleId, 目标指纹) 的检查将直接返回 Denied，
    //  从而实现 SRS 4.4 第 8 条「拒绝后不得通过其他路径重试」。
    // -----------------------------------------------------------------------
    void recordUserDecision(const PermissionRequest &request,
                            bool granted,
                            GrantScope scope);

    // -----------------------------------------------------------------------
    // 执行结果登记（SRS 4.4 第 7 条：执行后记录结果）
    // -----------------------------------------------------------------------
    void recordOutcome(const PermissionRequest &request,
                       const QString &result,
                       const QString &detail = QString(),
                       const QString &authorizationState = QString());

    // -----------------------------------------------------------------------
    // 授权管理（设置页「已授权操作」列表）
    // -----------------------------------------------------------------------
    QVector<GrantEntry> activeGrants() const;
    void revokeGrant(const QString &ruleId, const QString &targetFingerprint);
    void revokeAllGrantsForRule(const QString &ruleId);
    void revokeAllGrants();

    // 拒绝记录管理
    QVector<DenialRecord> activeDenials() const;
    void clearDenial(const QString &ruleId, const QString &targetFingerprint);
    void clearAllDenials();

    // -----------------------------------------------------------------------
    // 审计日志
    // -----------------------------------------------------------------------
    QVector<AuditRecord> recentAudit(int limit = 100) const;
    int auditCount() const;
    Result exportAudit(const QString &path) const;
    void clearAudit();

    // -----------------------------------------------------------------------
    // 风险等级判定（公开以便单元测试）
    //
    //  判定依据（按优先级）：
    //    1. 意图本身是否为禁止类
    //    2. 目标是否位于敏感位置（提升等级）
    //    3. 是否批量 / 不可逆操作（提升等级）
    //    4. 规则声明的风险等级作为基线
    // -----------------------------------------------------------------------
    static RiskLevel escalateRisk(RiskLevel baseline,
                                  SensitivityLevel sensitivity,
                                  bool batchOperation,
                                  bool irreversible);

    // 该意图是否属于「本程序不提供」的禁止类（SRS 4.4 禁止行）
    static bool isForbiddenIntent(const QString &intent);

    // 危险意图关键词清单（用于运行时兜底拦截，防止规则文件被改坏）
    static QStringList forbiddenIntentKeywords();

    // 目标指纹计算（多目标时取组合哈希）
    static QString computeTargetFingerprint(const PermissionRequest &request);

signals:
    // 需要界面弹出授权对话框时发出（UI 层连接此信号）
    void authorizationRequired(const dc::PermissionRequest &request);

    // 授权或拒绝记录发生变化
    void grantsChanged();

private:
    // 查询是否已有覆盖本次操作的授权
    bool hasGrant(const QString &ruleId, const QString &fingerprint,
                  GrantScope *scope = nullptr) const;

    // 查询是否处于拒绝锁定期
    bool isDenied(const QString &ruleId, const QString &fingerprint,
                  QString *reason = nullptr) const;

    // 清理过期条目
    void pruneExpired();

    // 审计记录的持久化
    void appendAudit(const AuditRecord &record);
    void loadAudit();
    void saveAudit() const;

    PlatformAdapter *m_adapter = nullptr;
    PermissionPolicy m_policy;

    QMap<QString, GrantEntry> m_grants;    // key = ruleId + "|" + fingerprint
    QMap<QString, DenialRecord> m_denials;

    QVector<AuditRecord> m_audit;
    static constexpr int kMaxInMemoryAudit = 2000;

    QString m_auditPath;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::GrantEntry)
Q_DECLARE_METATYPE(dc::PermissionRequest)
