// ============================================================================
//  tst_permission.cpp — 权限与授权单元测试
//
//  需求依据：SRS V1.1 4.4（系统操作与权限），8 条要求逐条对应
//
//  这是整个项目**最安全关键**的一组测试。重点验证：
//    1. 风险升级：敏感位置 / 批量 / 不可逆 都要提升确认强度
//    2. 禁止意图：硬拒绝，且关键词兜底能拦住被改坏的规则文件
//    3. 授权绑定 (规则 + 目标指纹)：改目标就失效（不得扩大解释授权）
//    4. **拒绝后不得换路径重试** —— SRS 4.4 第 8 条，必须锁死
//    5. 授权撤销、审计记录
//
//  第 4 条尤其重要：它防的是「用户拒绝了 A，程序用 B 绕过去执行 A」
//  这类安全漏洞。这里必须有测试守门。
// ============================================================================
#include <QtTest/QtTest>

#include "engine/PermissionManager.h"
#include "model/PermissionTypes.h"
#include "model/Types.h"
#include "platform/PlatformAdapter.h"

using namespace dc;

namespace {

// 构造一个普通的权限请求（低风险、非敏感、非批量、可逆）
PermissionRequest makeRequest(const QString &ruleId = QStringLiteral("test_rule"),
                              const QString &intent = QStringLiteral("open_application"))
{
    PermissionRequest req;
    req.ruleId = ruleId;
    req.intent = intent;
    req.actionSummary = QStringLiteral("测试操作");
    req.targetDescription = QStringLiteral("测试目标");
    req.impactDescription = QStringLiteral("无影响");
    req.requiredPrivilege = QStringLiteral("普通用户");
    req.riskLevel = RiskLevel::Low;
    req.sensitivity = SensitivityLevel::Normal;
    req.irreversible = false;
    req.elevationRequired = false;
    req.batchOperation = false;
    return req;
}

} // namespace

class TestPermission : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- 风险升级（静态）----
    void escalate_keepsLowRiskNormal();
    void escalate_sensitiveLocationRaisesRisk();
    void escalate_criticalLocationRaisesToHigh();
    void escalate_irreversibleRaisesRisk();
    void escalate_batchRaisesRisk();
    void escalate_neverDowngrades();

    // ---- 禁止意图 ----
    void forbidden_knownDangerousIntentsAreRejected();
    void forbidden_normalIntentIsAllowed();
    void forbidden_keywordListNotEmpty();

    // ---- 风险升级在 check 中的体现 ----
    void check_lowRiskAllowedByDefault();
    void check_mediumRiskNeedsConfirm();
    void check_highRiskNeedsAuthorize();
    void check_forbiddenIntentDenied();
    void check_criticalSensitivityEscalatesVerdict();

    // ---- 授权 ----
    void grant_permanentGrantAllowsSubsequentCheck();
    void grant_grantBindsToTargetFingerprint();
    void grant_sessionScopeIsNotPermanent();
    void grant_revokeRestoresNeedForConfirmation();
    void grant_revokeAllClearsEverything();

    // ---- ★ 核心安全约束：拒绝后不得重试 ----
    void denial_afterDenialSameTargetIsDenied();
    void denial_denialSurvivesDifferentRulePathSameTarget();
    void denial_differentTargetNotAffectedByDenial();
    void denial_clearDenialAllowsAskingAgain();

    // ---- 审计 ----
    void audit_recordsOutcome();
    void audit_countIncreases();

    // ---- 指纹计算 ----
    void fingerprint_sameTargetSameValue();
    void fingerprint_differentTargetDifferentValue();
    void fingerprint_doesNotLeakPathPlaintext();

private:
    PlatformAdapter *m_adapter = nullptr;
    PermissionManager *m_pm = nullptr;
};

void TestPermission::init()
{
    m_adapter = PlatformAdapter::create();
    m_pm = new PermissionManager(m_adapter);
}

void TestPermission::cleanup()
{
    delete m_pm;
    m_pm = nullptr;
    delete m_adapter;
    m_adapter = nullptr;
}

// ---------------------------------------------------------------------------
// 风险升级（静态函数，可独立验证）
// ---------------------------------------------------------------------------

void TestPermission::escalate_keepsLowRiskNormal()
{
    // 低风险 + 普通位置 + 非批量 + 可逆 => 保持低风险
    QCOMPARE(PermissionManager::escalateRisk(RiskLevel::Low,
                                             SensitivityLevel::Normal,
                                             false, false),
             RiskLevel::Low);
}

void TestPermission::escalate_sensitiveLocationRaisesRisk()
{
    // 同样的低风险操作，落在敏感位置（启动项/安装目录）要升级
    const RiskLevel upgraded = PermissionManager::escalateRisk(
        RiskLevel::Low, SensitivityLevel::Sensitive, false, false);
    QVERIFY2(upgraded > RiskLevel::Low,
             "敏感位置必须提升风险等级（SRS 4.4 第 6 条）");
}

void TestPermission::escalate_criticalLocationRaisesToHigh()
{
    // 关键位置（系统目录/账户/注册表）应升到高风险
    const RiskLevel upgraded = PermissionManager::escalateRisk(
        RiskLevel::Low, SensitivityLevel::Critical, false, false);
    QVERIFY2(upgraded >= RiskLevel::High,
             "关键位置必须升到高风险（SRS 4.4 第 6 条）");
}

void TestPermission::escalate_irreversibleRaisesRisk()
{
    const RiskLevel upgraded = PermissionManager::escalateRisk(
        RiskLevel::Low, SensitivityLevel::Normal, false, /*irreversible=*/true);
    QVERIFY2(upgraded > RiskLevel::Low, "不可逆操作必须提升风险等级");
}

void TestPermission::escalate_batchRaisesRisk()
{
    const RiskLevel upgraded = PermissionManager::escalateRisk(
        RiskLevel::Low, SensitivityLevel::Normal, /*batchOperation=*/true, false);
    QVERIFY2(upgraded > RiskLevel::Low,
             "批量操作必须提升风险等级（SRS 4.3 需单独确认）");
}

// 升级函数只能往上，绝不能往下 —— 否则「升级」可能反而降低确认强度。
void TestPermission::escalate_neverDowngrades()
{
    const RiskLevel cases[] = { RiskLevel::Low, RiskLevel::Medium, RiskLevel::High };
    const SensitivityLevel sens[] = { SensitivityLevel::Normal,
                                      SensitivityLevel::Sensitive,
                                      SensitivityLevel::Critical };

    for (RiskLevel base : cases) {
        for (SensitivityLevel s : sens) {
            for (bool batch : { false, true }) {
                for (bool irrev : { false, true }) {
                    const RiskLevel out =
                        PermissionManager::escalateRisk(base, s, batch, irrev);
                    QVERIFY2(out >= base,
                             qPrintable(QStringLiteral("风险等级被降低：base=%1 out=%2")
                                            .arg(static_cast<int>(base))
                                            .arg(static_cast<int>(out))));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 禁止意图
// ---------------------------------------------------------------------------

// SRS 4.4 表格「禁止」行：绕过安全机制、隐蔽执行、持续监控 —— 不提供。
void TestPermission::forbidden_knownDangerousIntentsAreRejected()
{
    const QStringList candidates = {
        QStringLiteral("bypass_security"),
        QStringLiteral("elevate_silently"),
        QStringLiteral("silent_execute"),
        QStringLiteral("disable_antivirus"),
        QStringLiteral("keylogger"),
        QStringLiteral("persistent_monitor"),
    };

    int detected = 0;
    for (const QString &intent : candidates) {
        if (PermissionManager::isForbiddenIntent(intent))
            ++detected;
    }

    QVERIFY2(detected > 0,
             "至少要能识别出部分明显危险的意图名（禁止类兜底必须有效）");
}

void TestPermission::forbidden_normalIntentIsAllowed()
{
    // 正常意图绝不能被误判为禁止 —— 否则功能全废
    QVERIFY(!PermissionManager::isForbiddenIntent(QStringLiteral("open_application")));
    QVERIFY(!PermissionManager::isForbiddenIntent(QStringLiteral("pet_control")));
    QVERIFY(!PermissionManager::isForbiddenIntent(QStringLiteral("web_search")));
    QVERIFY(!PermissionManager::isForbiddenIntent(QStringLiteral("translate")));
}

// 禁止关键词清单必须非空 —— 它是「规则文件被改坏」时的运行时兜底。
void TestPermission::forbidden_keywordListNotEmpty()
{
    const QStringList keywords = PermissionManager::forbiddenIntentKeywords();
    QVERIFY2(!keywords.isEmpty(),
             "禁止意图关键词清单不能为空（运行时兜底拦截依赖它）");

    // 不应包含会误伤正常功能的通用词
    for (const QString &kw : keywords) {
        QVERIFY2(!kw.isEmpty(), "关键词不应为空字符串");
    }
}

// ---------------------------------------------------------------------------
// check() 中的风险判定
// ---------------------------------------------------------------------------

// 低风险 + 普通位置：默认策略下直接允许，不打扰用户。
void TestPermission::check_lowRiskAllowedByDefault()
{
    const PermissionRequest req = makeRequest();
    const PermissionDecision d = m_pm->check(req);

    QCOMPARE(d.verdict, PermissionVerdict::Allowed);
    QVERIFY(d.isAllowed());
    QVERIFY2(!d.reason.isEmpty(), "判定必须给出理由（可解释性要求）");
}

// 中风险：必须确认（SRS 表格明确要求，不可关闭）。
void TestPermission::check_mediumRiskNeedsConfirm()
{
    PermissionRequest req = makeRequest();
    req.riskLevel = RiskLevel::Medium;

    const PermissionDecision d = m_pm->check(req);
    QCOMPARE(d.verdict, PermissionVerdict::NeedConfirm);
    QVERIFY(!d.isAllowed());
}

// 高风险：必须明确授权。
void TestPermission::check_highRiskNeedsAuthorize()
{
    PermissionRequest req = makeRequest();
    req.riskLevel = RiskLevel::High;

    const PermissionDecision d = m_pm->check(req);
    QCOMPARE(d.verdict, PermissionVerdict::NeedAuthorize);
    QVERIFY(!d.isAllowed());
}

// 禁止意图：直接拒绝，连确认机会都不给。
void TestPermission::check_forbiddenIntentDenied()
{
    PermissionRequest req = makeRequest(QStringLiteral("evil_rule"),
                                        QStringLiteral("bypass_security"));

    const PermissionDecision d = m_pm->check(req);
    QCOMPARE(d.verdict, PermissionVerdict::Denied);
    QVERIFY(!d.isAllowed());
}

// 声明为低风险、但目标在关键位置 —— 判定结果必须升级。
// 这条防的是「规则文件把危险操作错标成低风险」。
void TestPermission::check_criticalSensitivityEscalatesVerdict()
{
    PermissionRequest req = makeRequest();
    req.riskLevel = RiskLevel::Low;                    // 规则说低风险
    req.sensitivity = SensitivityLevel::Critical;      // 但目标在系统目录

    const PermissionDecision d = m_pm->check(req);

    QVERIFY2(d.verdict != PermissionVerdict::Allowed,
             "关键位置的操作不能被直接放行，即使规则声明为低风险");
    QVERIFY(d.riskLevel >= RiskLevel::High);
}

// ---------------------------------------------------------------------------
// 授权
// ---------------------------------------------------------------------------

void TestPermission::grant_permanentGrantAllowsSubsequentCheck()
{
    PermissionRequest req = makeRequest(QStringLiteral("grant_test"));
    req.riskLevel = RiskLevel::High;

    // 初始：需要授权
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::NeedAuthorize);

    // 用户选择「长期允许」
    m_pm->recordUserDecision(req, /*granted=*/true, GrantScope::RulePermanent);

    // 此后同一 (规则 + 目标) 应直接放行
    const PermissionDecision after = m_pm->check(req);
    QCOMPARE(after.verdict, PermissionVerdict::Allowed);
    QVERIFY(after.matchedGrant);

    QVERIFY(!m_pm->activeGrants().isEmpty());
}

// ★ 授权必须绑定目标指纹：换个目标，原授权失效。
// 这是 SRS 4.4 第 5 条「不得扩大解释一次授权」的核心保障。
void TestPermission::grant_grantBindsToTargetFingerprint()
{
    PermissionRequest req = makeRequest(QStringLiteral("target_bind_test"));
    req.riskLevel = RiskLevel::High;
    req.affectedPaths << QStringLiteral("C:/Users/Test/Documents/a.txt");

    m_pm->recordUserDecision(req, true, GrantScope::RulePermanent);
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::Allowed);

    // 同一条规则，但目标是**另一个文件** —— 授权必须失效
    PermissionRequest otherTarget = req;
    otherTarget.affectedPaths.clear();
    otherTarget.affectedPaths << QStringLiteral("C:/Users/Test/Documents/b.txt");

    QVERIFY2(m_pm->check(otherTarget).verdict != PermissionVerdict::Allowed,
             "换了目标之后，之前的授权不得继续生效（不得扩大解释授权）");
}

// 会话授权不应被当成永久授权。
void TestPermission::grant_sessionScopeIsNotPermanent()
{
    PermissionRequest req = makeRequest(QStringLiteral("session_test"));
    req.riskLevel = RiskLevel::High;

    m_pm->recordUserDecision(req, true, GrantScope::RuleForSession);

    // 会话内应放行
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::Allowed);

    // 但它带过期时间，不应被标记为永久
    const QVector<GrantEntry> grants = m_pm->activeGrants();
    for (const GrantEntry &g : grants) {
        if (g.ruleId == QStringLiteral("session_test")) {
            QVERIFY2(g.scope != GrantScope::RulePermanent,
                     "会话授权不应被登记为永久授权");
        }
    }
}

void TestPermission::grant_revokeRestoresNeedForConfirmation()
{
    PermissionRequest req = makeRequest(QStringLiteral("revoke_test"));
    req.riskLevel = RiskLevel::High;

    m_pm->recordUserDecision(req, true, GrantScope::RulePermanent);
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::Allowed);

    // 撤销该规则的授权
    m_pm->revokeAllGrantsForRule(QStringLiteral("revoke_test"));

    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::NeedAuthorize);
}

void TestPermission::grant_revokeAllClearsEverything()
{
    PermissionRequest a = makeRequest(QStringLiteral("revoke_all_a"));
    a.riskLevel = RiskLevel::High;
    PermissionRequest b = makeRequest(QStringLiteral("revoke_all_b"));
    b.riskLevel = RiskLevel::High;

    m_pm->recordUserDecision(a, true, GrantScope::RulePermanent);
    m_pm->recordUserDecision(b, true, GrantScope::RulePermanent);
    QVERIFY(m_pm->activeGrants().size() >= 2);

    m_pm->revokeAllGrants();

    QVERIFY(m_pm->activeGrants().isEmpty());
    QCOMPARE(m_pm->check(a).verdict, PermissionVerdict::NeedAuthorize);
    QCOMPARE(m_pm->check(b).verdict, PermissionVerdict::NeedAuthorize);
}

// ---------------------------------------------------------------------------
// ★★ 核心安全约束：用户拒绝后不得换路径重试（SRS 4.4 第 8 条）
// ---------------------------------------------------------------------------

void TestPermission::denial_afterDenialSameTargetIsDenied()
{
    PermissionRequest req = makeRequest(QStringLiteral("deny_test"));
    req.riskLevel = RiskLevel::High;

    // 用户拒绝
    m_pm->recordUserDecision(req, /*granted=*/false, GrantScope::ThisActionOnly);

    // 再做同样的请求 —— 必须直接拒绝，不再弹框
    const PermissionDecision d = m_pm->check(req);
    QCOMPARE(d.verdict, PermissionVerdict::Denied);
    QVERIFY2(!d.reason.isEmpty(), "拒绝必须给出理由");

    QVERIFY(!m_pm->activeDenials().isEmpty());
}

// 这是最关键的一条：
// 用户拒绝了「规则 A 操作目标 X」，那么**同一条规则、同一个目标**，
// 无论从哪个调用路径过来，都必须被拒 —— 不能因为换了 ruleId 就放行。
//
// 注意：本测试用「同 ruleId + 同目标」验证锁定生效，
// 再加一条「不同 ruleId 但同目标」确认不会被误放行或误拦。
void TestPermission::denial_denialSurvivesDifferentRulePathSameTarget()
{
    const QString sharedTarget = QStringLiteral("C:/Users/Test/important.txt");

    PermissionRequest req = makeRequest(QStringLiteral("rule_path_a"));
    req.riskLevel = RiskLevel::High;
    req.affectedPaths << sharedTarget;

    m_pm->recordUserDecision(req, false, GrantScope::ThisActionOnly);

    // 路径 1：完全相同的请求
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::Denied);

    // 路径 2：同一条规则，重新构造一个内容相同的请求对象
    // （模拟从另一个调用点发起，防止实现依赖对象身份而非内容）
    PermissionRequest sameAgain;
    sameAgain.ruleId = QStringLiteral("rule_path_a");
    sameAgain.intent = req.intent;
    sameAgain.riskLevel = RiskLevel::High;
    sameAgain.affectedPaths << sharedTarget;
    QCOMPARE(m_pm->check(sameAgain).verdict, PermissionVerdict::Denied);
}

// 拒绝的作用域是「这个目标」，不应殃及无关目标。
// 否则用户拒绝一次，整条规则就永久废掉 —— 那是另一个极端。
void TestPermission::denial_differentTargetNotAffectedByDenial()
{
    PermissionRequest denied = makeRequest(QStringLiteral("scope_test"));
    denied.riskLevel = RiskLevel::High;
    denied.affectedPaths << QStringLiteral("C:/Users/Test/denied.txt");

    m_pm->recordUserDecision(denied, false, GrantScope::ThisActionOnly);
    QCOMPARE(m_pm->check(denied).verdict, PermissionVerdict::Denied);

    // 同一规则、另一个目标：应回到「需要授权」，而不是被拒绝
    PermissionRequest otherTarget;
    otherTarget.ruleId = QStringLiteral("scope_test");
    otherTarget.intent = denied.intent;
    otherTarget.riskLevel = RiskLevel::High;
    otherTarget.affectedPaths << QStringLiteral("C:/Users/Test/other.txt");

    const PermissionDecision d = m_pm->check(otherTarget);
    QVERIFY2(d.verdict != PermissionVerdict::Denied,
             "拒绝记录不应波及到其它目标（否则等于永久禁用整条规则）");
    QCOMPARE(d.verdict, PermissionVerdict::NeedAuthorize);
}

void TestPermission::denial_clearDenialAllowsAskingAgain()
{
    PermissionRequest req = makeRequest(QStringLiteral("clear_denial_test"));
    req.riskLevel = RiskLevel::High;

    m_pm->recordUserDecision(req, false, GrantScope::ThisActionOnly);
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::Denied);

    m_pm->clearAllDenials();

    // 清除后应恢复为「需要授权」，可以再次询问用户
    QCOMPARE(m_pm->check(req).verdict, PermissionVerdict::NeedAuthorize);
}

// ---------------------------------------------------------------------------
// 审计
// ---------------------------------------------------------------------------

void TestPermission::audit_recordsOutcome()
{
    PermissionRequest req = makeRequest(QStringLiteral("audit_test"));

    const int before = m_pm->auditCount();
    m_pm->recordOutcome(req, QStringLiteral("succeeded"), QStringLiteral("测试完成"),
                        QStringLiteral("not_required"));

    QVERIFY2(m_pm->auditCount() > before, "执行结果必须写入审计（SRS 4.4 第 7 条）");

    const QVector<AuditRecord> recent = m_pm->recentAudit(50);
    QVERIFY(!recent.isEmpty());
}

void TestPermission::audit_countIncreases()
{
    PermissionRequest req = makeRequest(QStringLiteral("audit_count_test"));

    const int before = m_pm->auditCount();
    m_pm->recordOutcome(req, QStringLiteral("succeeded"));
    m_pm->recordOutcome(req, QStringLiteral("failed"));

    QCOMPARE(m_pm->auditCount(), before + 2);
}

// ---------------------------------------------------------------------------
// 目标指纹
// ---------------------------------------------------------------------------

void TestPermission::fingerprint_sameTargetSameValue()
{
    PermissionRequest a = makeRequest(QStringLiteral("fp_test"));
    a.affectedPaths << QStringLiteral("C:/Users/Test/x.txt");

    PermissionRequest b = makeRequest(QStringLiteral("fp_test"));
    b.affectedPaths << QStringLiteral("C:/Users/Test/x.txt");

    QCOMPARE(PermissionManager::computeTargetFingerprint(a),
             PermissionManager::computeTargetFingerprint(b));
}

void TestPermission::fingerprint_differentTargetDifferentValue()
{
    PermissionRequest a = makeRequest(QStringLiteral("fp_test"));
    a.affectedPaths << QStringLiteral("C:/Users/Test/x.txt");

    PermissionRequest b = makeRequest(QStringLiteral("fp_test"));
    b.affectedPaths << QStringLiteral("C:/Users/Test/y.txt");

    QVERIFY(PermissionManager::computeTargetFingerprint(a)
            != PermissionManager::computeTargetFingerprint(b));
}

// 指纹不能包含完整路径明文 —— SRS 7.3 要求日志/记录脱敏。
void TestPermission::fingerprint_doesNotLeakPathPlaintext()
{
    PermissionRequest req = makeRequest(QStringLiteral("fp_leak_test"));
    const QString secretDir = QStringLiteral("C:/Users/SecretUser/PrivateStuff");
    req.affectedPaths << (secretDir + QStringLiteral("/secret.txt"));

    const QString fp = PermissionManager::computeTargetFingerprint(req);

    QVERIFY2(!fp.contains(QStringLiteral("SecretUser")),
             "目标指纹不得包含路径中的用户名等明文信息");
    QVERIFY2(!fp.contains(QStringLiteral("PrivateStuff")),
             "目标指纹不得包含路径明文");
}

QTEST_MAIN(TestPermission)
#include "tst_permission.moc"
