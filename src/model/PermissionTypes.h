// ============================================================================
//  PermissionTypes.h — 权限与授权数据结构
//
//  需求依据：SRS V1.1 第 4.4 节（系统操作与权限）
//
//  关键约束（逐条落到数据结构上）：
//    1. 默认以普通用户权限运行          -> PermissionRequest::elevationRequired
//    2. 不得暗中取得管理员权限          -> 结构体中无任何自动提权字段
//    3. 高权限操作展示目标/影响/所需权限 -> PermissionRequest 三个描述字段
//    4. 「记住授权」只能针对具体操作类别 -> GrantScope 枚举，非全局开关
//    5. 不得扩大解释一次授权            -> ThisActionOnly 为默认值
//    6. 敏感位置更严格确认              -> SensitivityLevel 独立于 RiskLevel
//    7. 执行前验证、执行后记录          -> PermissionDecision + 审计记录
//    8. 用户拒绝后不得换路径重试        -> DenialRecord（持久化拒绝记录）
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 敏感位置等级（SRS 4.4 第 6 条）
//   独立于 RiskLevel 存在：即使某个操作被判为「中等风险」，
//   只要目标落在系统目录/启动项/账户/网络安全配置上，就要升级确认强度。
// ---------------------------------------------------------------------------
enum class SensitivityLevel {
    Normal = 0,     // 用户目录下的普通位置
    Sensitive,      // 启动项、用户配置文件、程序安装目录
    Critical        // 系统目录、账户、注册表、网络安全配置
};

QString sensitivityLevelToString(SensitivityLevel level);

// ---------------------------------------------------------------------------
// 授权范围（SRS 4.4 第 4、5 条）
// ---------------------------------------------------------------------------
enum class GrantScope {
    ThisActionOnly = 0,   // 只针对本次操作（默认，最小授权）
    RuleForSession,       // 本会话内该规则始终允许（会话结束失效）
    RulePermanent         // 该规则长期允许（写盘，可在设置中撤销）
};

QString grantScopeToString(GrantScope scope);
GrantScope grantScopeFromString(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------------------
// 权限请求：一次待授权操作的完整描述
//   字段设计对应 SRS 4.4 第 3 条「展示操作目标、影响范围和所需权限」
// ---------------------------------------------------------------------------
struct PermissionRequest {
    QString ruleId;              // 触发本次操作的命令规则
    QString intent;              // 意图
    QString actionSummary;       // 一句话说明要做什么（面向用户）
    QString targetDescription;   // 操作目标，如具体路径/程序名
    QString impactDescription;   // 影响范围说明
    QString requiredPrivilege;   // 所需权限，如「普通用户」/「管理员」
    RiskLevel riskLevel = RiskLevel::Low;
    SensitivityLevel sensitivity = SensitivityLevel::Normal;
    QStringList affectedPaths;   // 涉及的具体路径（用于展示与审计）
    QStringList affectedTargets; // 涉及的注册表键/服务名等
    bool irreversible = false;   // 是否不可逆（删除、覆盖等）
    bool elevationRequired = false;  // 是否必须由系统安全提示提升权限
    bool batchOperation = false;     // 是否批量操作（SRS 4.3 需单独确认）

    // 规则自身声明的「即使低风险也要确认」（SRS 4.6 规则 JSON 的
    // confirmation 字段）。用函数而非裸字段，是为了让「哪些情况必须确认」
    // 这条判断只有一个来源，避免各调用点各写一套。
    bool ruleRequiresConfirmation = false;

    // 综合判定：本请求是否必须经过确认/授权流程
    bool requiresConfirmation() const
    {
        return ruleRequiresConfirmation || irreversible || batchOperation;
    }

    QJsonObject toJson() const;
    // 用于日志：不含完整敏感路径（SRS 7.3）
    QString toSafeLogString() const;
};

// ---------------------------------------------------------------------------
// 权限决策结果
// ---------------------------------------------------------------------------
struct PermissionDecision {
    PermissionVerdict verdict = PermissionVerdict::Denied;
    QString reason;                       // 判定理由，直接展示给用户
    RiskLevel riskLevel = RiskLevel::Low; // 回带本次判定的风险等级
    SensitivityLevel sensitivity = SensitivityLevel::Normal;
    bool matchedGrant = false;            // 是否命中了已有授权
    GrantScope matchedScope = GrantScope::ThisActionOnly;

    bool isAllowed() const { return verdict == PermissionVerdict::Allowed; }
};

// ---------------------------------------------------------------------------
// 拒绝记录（SRS 4.4 第 8 条）
//   「用户取消或拒绝授权后，不得通过其他路径重试同一高风险操作」
//   实现方式：用户拒绝后写入本记录，同一 ruleId + 目标指纹在有效期内
//   直接返回 Denied，不再弹出授权对话框，也不允许其他调用路径绕过。
// ---------------------------------------------------------------------------
struct DenialRecord {
    QString ruleId;
    QString targetFingerprint;   // 目标指纹（路径哈希等），保证同一目标才算同一次
    QDateTime deniedAt;
    QDateTime expiresAt;
    QString reason;

    bool isActive(const QDateTime &now = QDateTime::currentDateTime()) const
    {
        return !expiresAt.isValid() || now < expiresAt;
    }

    QJsonObject toJson() const;
    static DenialRecord fromJson(const QJsonObject &obj);
};

// ---------------------------------------------------------------------------
// 审计记录（SRS 7.3：重要操作需记录类型、时间、结果和授权状态）
// ---------------------------------------------------------------------------
struct AuditRecord {
    QDateTime timestamp;
    QString ruleId;
    QString intent;
    RiskLevel riskLevel = RiskLevel::Low;
    SensitivityLevel sensitivity = SensitivityLevel::Normal;
    QString authorizationState;   // "not_required" / "granted_once" / "granted_scope" / "denied"
    QString result;               // "succeeded" / "failed" / "cancelled"
    QString targetSummary;        // 脱敏后的目标摘要
    QString detail;

    QJsonObject toJson() const;
};

} // namespace dc
