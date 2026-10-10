// ============================================================================
//  tst_command_repo.cpp — 命令规则仓库单元测试
//
//  需求依据：SRS V1.1 4.6（命令匹配、参数提取、别名和优先级、冲突校验）
//
//  测试重点：
//    1. 从 resources/commands/ 加载三语言规则，数量与别名表符合预期
//    2. validate() 不报「错误」级问题（占位符与参数声明必须匹配）
//    3. 别名表查询与归一化
//    4. 冲突检测能报告但不阻止加载
//    5. 增删规则、按意图/语言查询
//    6. CommandRule 自身的校验逻辑
//
//  这是「规则文件写错了能不能发现」的守门测试。第一轮开发时正是这里
//  报出了 6 条「引用了未声明的占位符」错误，所以必须长期覆盖。
// ============================================================================
#include <QtTest/QtTest>

#include "engine/CommandRepository.h"
#include "model/CommandRule.h"
#include "model/Types.h"

using namespace dc;

namespace {

QString commandsDir()
{
    return QString::fromUtf8(DC_TEST_RESOURCE_DIR) + QStringLiteral("/commands");
}

} // namespace

class TestCommandRepo : public QObject
{
    Q_OBJECT

private slots:
    // ---- 加载 ----
    void load_realRuleDirectorySucceeds();
    void load_coversThreeLanguages();
    void load_hasAliasTables();
    void load_missingDirectoryFailsGracefully();

    // ---- 校验 ----
    void validate_noErrorLevelIssues();

    // ---- 别名 ----
    void alias_normalizesKnownValue();
    void alias_unknownValueReturnsInput();
    void alias_missingTableReturnsInput();
    void alias_hasTableQueries();

    // ---- 查询 ----
    void query_countMatchesLoaded();
    void query_ruleById();
    void query_rulesByIntent();
    void query_enabledRulesFilterDisabled();

    // ---- 增删 ----
    void mutate_addAndRemoveRule();
    void mutate_clearEmptiesRepository();

    // ---- 冲突 ----
    void conflict_detectsIdenticalPatternsWithSamePriority();

    // ---- 单条规则校验 ----
    void ruleValidation_undeclaredPlaceholderIsError();
    void ruleValidation_validRulePasses();
};

// ---------------------------------------------------------------------------
// 加载
// ---------------------------------------------------------------------------

void TestCommandRepo::load_realRuleDirectorySucceeds()
{
    CommandRepository repo;
    const RuleLoadResult r = repo.loadFromDirectory(commandsDir());

    QVERIFY2(r.ok, qPrintable(QStringLiteral("加载失败：%1")
                                  .arg(r.errorMessages().join(QStringLiteral("; ")))));
    QVERIFY(r.loadedRuleCount > 0);
    QVERIFY(!r.sourceFiles.isEmpty());
    QCOMPARE(repo.count(), r.loadedRuleCount);
}

// 三种语言都要覆盖到 —— 否则某个语言下用户发现桌宠「听不懂话」。
void TestCommandRepo::load_coversThreeLanguages()
{
    CommandRepository repo;
    const RuleLoadResult r = repo.loadFromDirectory(commandsDir());
    QVERIFY(r.ok);

    const QHash<Language, int> byLang = repo.countByLanguage();

    // 三语言都应有规则。注意 Language::Auto 表示「与语言无关」的规则，
    // 它不参与这里的判断。
    int total = 0;
    for (auto it = byLang.constBegin(); it != byLang.constEnd(); ++it) {
        total += it.value();
    }
    QVERIFY2(total > 0, "规则语言统计为空");

    QVERIFY2(r.loadedRuleCount >= 16,
             qPrintable(QStringLiteral("规则数偏少（%1），可能有文件加载失败")
                            .arg(r.loadedRuleCount)));
}

void TestCommandRepo::load_hasAliasTables()
{
    CommandRepository repo;
    const RuleLoadResult r = repo.loadFromDirectory(commandsDir());
    QVERIFY(r.ok);

    QVERIFY2(r.loadedAliasTables >= 1, "未加载到别名表");

    // 真实规则文件里定义了 app / folder / search_engine 三张表
    const QStringList tables = repo.aliasTables();
    QVERIFY2(tables.contains(QStringLiteral("app")),
             qPrintable(QStringLiteral("缺少 app 别名表，实际有：%1")
                            .arg(tables.join(QStringLiteral(", ")))));
}

// 目录不存在时应优雅失败，而不是崩溃或抛异常。
void TestCommandRepo::load_missingDirectoryFailsGracefully()
{
    CommandRepository repo;
    const RuleLoadResult r = repo.loadFromDirectory(
        QStringLiteral("Z:/definitely/not/a/real/path/xyz"));

    QVERIFY(!r.ok);
    QCOMPARE(repo.count(), 0);
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

// 关键守门用例：真实规则文件里不能有「错误」级问题。
//
// 「引用了未声明的占位符」属于 Error 级 —— 那意味着某条规则的模式永远
// 匹配不上，用户说了话桌宠也没反应。第一轮开发踩过这个坑，必须防复发。
void TestCommandRepo::validate_noErrorLevelIssues()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    const QVector<RuleIssue> issues = repo.validate();

    QStringList errors;
    for (const RuleIssue &issue : issues) {
        if (issue.severity == RuleIssue::Severity::Error) {
            errors << QStringLiteral("[%1] %2").arg(issue.ruleId, issue.message);
        }
    }

    QVERIFY2(errors.isEmpty(),
             qPrintable(QStringLiteral("规则文件存在 %1 条错误级问题：\n%2")
                            .arg(errors.size())
                            .arg(errors.join(QStringLiteral("\n")))));
}

// ---------------------------------------------------------------------------
// 别名
// ---------------------------------------------------------------------------

void TestCommandRepo::alias_normalizesKnownValue()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    // 「浏览器」应归一化为 browser
    QCOMPARE(repo.normalizeAlias(QStringLiteral("app"), QStringLiteral("浏览器")),
             QStringLiteral("browser"));

    // 「桌面」应归一化为 desktop
    QCOMPARE(repo.normalizeAlias(QStringLiteral("folder"), QStringLiteral("桌面")),
             QStringLiteral("desktop"));
}

// 别名表里没有的值按设计原样返回 —— 表示「这个输入本身就是标准值」。
// 这样调用方不需要区分「查不到」和「本来就是标准值」。
void TestCommandRepo::alias_unknownValueReturnsInput()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    const QString unknown = QStringLiteral("完全不存在的别名xyz");
    QCOMPARE(repo.normalizeAlias(QStringLiteral("app"), unknown), unknown);
}

void TestCommandRepo::alias_missingTableReturnsInput()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    const QString value = QStringLiteral("browser");
    QCOMPARE(repo.normalizeAlias(QStringLiteral("no_such_table"), value), value);
}

void TestCommandRepo::alias_hasTableQueries()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    QVERIFY(repo.hasAliasTable(QStringLiteral("app")));
    QVERIFY(!repo.hasAliasTable(QStringLiteral("no_such_table")));

    const QMap<QString, QString> appTable = repo.aliasTable(QStringLiteral("app"));
    QVERIFY(!appTable.isEmpty());
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

void TestCommandRepo::query_countMatchesLoaded()
{
    CommandRepository repo;
    const RuleLoadResult r = repo.loadFromDirectory(commandsDir());
    QCOMPARE(repo.count(), r.loadedRuleCount);
    QVERIFY(repo.count() > 0);
}

void TestCommandRepo::query_ruleById()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    QVERIFY(repo.contains(QStringLiteral("open_browser")));

    const CommandRule rule = repo.ruleById(QStringLiteral("open_browser"));
    QCOMPARE(rule.id, QStringLiteral("open_browser"));
    QCOMPARE(rule.intent, QStringLiteral("open_application"));
    QVERIFY(!rule.patterns.isEmpty());

    // 不存在的 id 返回空规则
    const CommandRule missing = repo.ruleById(QStringLiteral("no_such_rule"));
    QVERIFY(missing.id.isEmpty());
}

void TestCommandRepo::query_rulesByIntent()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    // open_application 意图下至少有 open_browser / open_application_by_name
    const QVector<CommandRule> rules =
        repo.rulesByIntent(QStringLiteral("open_application"));
    QVERIFY2(!rules.isEmpty(), "open_application 意图下应当有规则");

    for (const CommandRule &r : rules) {
        QCOMPARE(r.intent, QStringLiteral("open_application"));
    }
}

void TestCommandRepo::query_enabledRulesFilterDisabled()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);

    CommandRule disabled;
    disabled.id = QStringLiteral("test_disabled_rule");
    disabled.intent = QStringLiteral("help");
    disabled.patterns << QStringLiteral("测试禁用规则");
    disabled.enabled = false;
    repo.addRule(disabled);

    const QVector<CommandRule> enabled = repo.enabledRules();
    for (const CommandRule &r : enabled) {
        QVERIFY2(r.enabled, qPrintable(QStringLiteral("禁用的规则 %1 出现在 enabledRules 里")
                                           .arg(r.id)));
    }

    // 禁用的规则仍然能被 ruleById 查到（只是不参与匹配）
    QVERIFY(repo.contains(QStringLiteral("test_disabled_rule")));
}

// ---------------------------------------------------------------------------
// 增删
// ---------------------------------------------------------------------------

void TestCommandRepo::mutate_addAndRemoveRule()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);
    const int before = repo.count();

    CommandRule custom;
    custom.id = QStringLiteral("my_custom_rule");
    custom.intent = QStringLiteral("help");
    custom.patterns << QStringLiteral("我的自定义命令");
    custom.riskLevel = RiskLevel::Low;
    repo.addRule(custom);

    QCOMPARE(repo.count(), before + 1);
    QVERIFY(repo.contains(QStringLiteral("my_custom_rule")));

    repo.removeRule(QStringLiteral("my_custom_rule"));
    QCOMPARE(repo.count(), before);
    QVERIFY(!repo.contains(QStringLiteral("my_custom_rule")));
}

void TestCommandRepo::mutate_clearEmptiesRepository()
{
    CommandRepository repo;
    QVERIFY(repo.loadFromDirectory(commandsDir()).ok);
    QVERIFY(repo.count() > 0);

    repo.clear();
    QCOMPARE(repo.count(), 0);
    QVERIFY(repo.enabledRules().isEmpty());
}

// ---------------------------------------------------------------------------
// 冲突检测
// ---------------------------------------------------------------------------

// 两条同优先级、模式串完全一致的规则必然产生歧义。
// 检测出来了但要允许加载 —— 只是提示配置有问题（SRS 4.6 要求「提供配置校验」，
// 不是「拒绝启动」）。
void TestCommandRepo::conflict_detectsIdenticalPatternsWithSamePriority()
{
    CommandRepository repo;

    CommandRule a;
    a.id = QStringLiteral("conflict_a");
    a.intent = QStringLiteral("help");
    a.patterns << QStringLiteral("打开冲突测试");
    a.priority = 5;

    CommandRule b;
    b.id = QStringLiteral("conflict_b");
    b.intent = QStringLiteral("help");
    b.patterns << QStringLiteral("打开冲突测试");   // 完全相同的模式
    b.priority = 5;                                // 相同优先级

    repo.addRule(a);
    repo.addRule(b);

    const QVector<RuleConflict> conflicts = repo.detectConflicts();
    QVERIFY2(!conflicts.isEmpty(), "相同模式 + 相同优先级应当被检测为冲突");
}

// ---------------------------------------------------------------------------
// 单条规则校验
// ---------------------------------------------------------------------------

// 模式里写了 {app}，但参数里没有对应的 source:slot 声明 —— 必须报错。
// 这正是第一轮开发时踩的坑。
void TestCommandRepo::ruleValidation_undeclaredPlaceholderIsError()
{
    CommandRule rule;
    rule.id = QStringLiteral("bad_rule");
    rule.intent = QStringLiteral("open_application");
    rule.patterns << QStringLiteral("打开{app}");   // 有占位符
    // 故意不声明任何参数

    QVERIFY(!rule.isValid());
    const QString err = rule.validationError();
    QVERIFY2(!err.isEmpty(), "非法规则必须给出原因");
}

void TestCommandRepo::ruleValidation_validRulePasses()
{
    CommandRule rule;
    rule.id = QStringLiteral("good_rule");
    rule.intent = QStringLiteral("open_application");
    rule.patterns << QStringLiteral("打开{app}");

    ParameterTemplate slotParam;
    slotParam.name = QStringLiteral("app_raw");
    slotParam.source = ParameterSource::Slot;
    slotParam.slotName = QStringLiteral("app");
    slotParam.required = true;
    rule.parameters << slotParam;

    QVERIFY2(rule.isValid(),
             qPrintable(QStringLiteral("合法规则被判为非法：%1").arg(rule.validationError())));
    QVERIFY(rule.validationError().isEmpty());
}

QTEST_MAIN(TestCommandRepo)
#include "tst_command_repo.moc"
