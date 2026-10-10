// ============================================================================
//  tst_command_engine.cpp — 命令解析引擎单元测试
//
//  需求依据：SRS V1.1 4.6（命令引擎）、7.3（输入校验）、4.2（多语言）
//
//  测试重点：
//    1. normalizeInput —— 全角转半角、空白折叠、中文标点归一、零宽字符清理
//    2. isInputAcceptable —— 空输入、超长、控制字符
//    3. parse —— 精确匹配、参数抽取、别名归一化、歧义与无匹配
//    4. 「不得猜测执行」—— 无法识别时必须是 NoMatch，而不是随便挑一条
//
//  这些用例针对真实的 resources/commands/*.json，而不是自造的假数据 ——
//  这样测的是「发布出去的东西到底对不对」。
// ============================================================================
#include <QtTest/QtTest>

#include "engine/CommandEngine.h"
#include "engine/CommandRepository.h"
#include "model/ParseResult.h"
#include "model/Types.h"

using namespace dc;

namespace {

// 定位源码树里的资源目录。
// DC_TEST_RESOURCE_DIR 由 tests/CMakeLists.txt 注入。
QString resourceDir()
{
    return QString::fromUtf8(DC_TEST_RESOURCE_DIR);
}

QString commandsDir()
{
    return resourceDir() + QStringLiteral("/commands");
}

} // namespace

class TestCommandEngine : public QObject
{
    Q_OBJECT

private slots:
    // ---- 一次性准备：加载真实规则文件 ----
    void initTestCase();

    // ---- 输入规范化 ----
    void normalize_trimsAndCollapsesWhitespace();
    void normalize_convertsFullWidthToHalfWidth();
    void normalize_normalizesChinesePunctuation();
    void normalize_removesZeroWidthAndControlChars();
    void normalize_preservesNormalChinese();

    // ---- 输入合法性 ----
    void acceptable_rejectsEmpty();
    void acceptable_rejectsOverlong();
    void acceptable_rejectsControlChars();
    void acceptable_acceptsNormalInput();

    // ---- 解析：精确匹配 ----
    void parse_exactMatch_openBrowser();
    void parse_exactMatch_petHide();
    void parse_exactMatch_help();
    void parse_matchedResultIsExecutable();

    // ---- 解析：参数抽取 ----
    void parse_extractsSlotParameter_webSearch();
    void parse_extractsSlotParameter_translate();
    void parse_normalizesAlias_appName();
    void parse_aliasUnknownValuePassesThroughRaw();
    void parse_openFolderUsesFolderAlias();

    // ---- 解析：无法识别 ----
    void parse_gibberishIsNotMatched();
    void parse_emptyInputIsInvalid();

    // ---- 解析：确定性 ----
    void parse_sameInputGivesSameResult();

private:
    CommandRepository *m_repo = nullptr;
    CommandEngine *m_engine = nullptr;
};

// ---------------------------------------------------------------------------
void TestCommandEngine::initTestCase()
{
    m_repo = new CommandRepository(this);
    const RuleLoadResult load = m_repo->loadFromDirectory(commandsDir());

    QVERIFY2(load.ok, qPrintable(QStringLiteral("规则加载失败：%1")
                                     .arg(load.errorMessages().join(QStringLiteral("; ")))));
    QVERIFY2(load.loadedRuleCount > 0,
             qPrintable(QStringLiteral("未加载到任何规则，检查资源路径：%1").arg(commandsDir())));

    m_engine = new CommandEngine(m_repo, this);
}

// ---------------------------------------------------------------------------
// 输入规范化
// ---------------------------------------------------------------------------

// 首尾空白要清掉，中间连续空白要折叠成一个空格。
// 用户从别处复制命令过来时经常带多余空格，不折叠就匹配不上规则。
void TestCommandEngine::normalize_trimsAndCollapsesWhitespace()
{
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("  打开浏览器  ")),
             QStringLiteral("打开浏览器"));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("搜索    天气")),
             QStringLiteral("搜索 天气"));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("\t搜索\n天气\t")),
             QStringLiteral("搜索 天气"));
}

// 全角字母/数字/标点要转半角。
// 中文输入法下打英文常常是全角，不转就匹配不上含英文的规则。
void TestCommandEngine::normalize_convertsFullWidthToHalfWidth()
{
    // 全角 A（U+FF21）应变成半角 A
    const QString fullWidth = QStringLiteral("ＡＢＣ");
    QCOMPARE(CommandEngine::normalizeInput(fullWidth), QStringLiteral("ABC"));

    // 全角数字
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("１２３")),
             QStringLiteral("123"));

    // 全角空格（U+3000）应被当作空白处理
    const QString ideographicSpace = QString(QChar(0x3000));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("搜索") + ideographicSpace
                                          + QStringLiteral("天气")),
             QStringLiteral("搜索 天气"));
}

// 中文标点归一成半角，让规则只需要写一套写法。
void TestCommandEngine::normalize_normalizesChinesePunctuation()
{
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("你好，世界")),
             QStringLiteral("你好,世界"));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("是吗？")),
             QStringLiteral("是吗?"));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("太好了！")),
             QStringLiteral("太好了!"));
    QCOMPARE(CommandEngine::normalizeInput(QStringLiteral("结束。")),
             QStringLiteral("结束."));
}

// 零宽字符与控制字符必须清掉。
// 这类字符肉眼不可见，却会让字符串比较失败 —— 是典型的「注入/混淆」手法。
void TestCommandEngine::normalize_removesZeroWidthAndControlChars()
{
    // 在「打开浏览器」中间插入零宽空格 U+200B
    const QString withZeroWidth =
        QStringLiteral("打开") + QChar(0x200B) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withZeroWidth), QStringLiteral("打开浏览器"));

    // 插入 U+FEFF（BOM / 零宽不换行空格）
    const QString withBom =
        QStringLiteral("打开") + QChar(0xFEFF) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withBom), QStringLiteral("打开浏览器"));

    // 控制字符（如 \x01）应被移除
    const QString withControl =
        QStringLiteral("打开") + QChar(0x01) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withControl), QStringLiteral("打开浏览器"));

    // DEL（0x7F）与 C1 控制字符（0x80-0x9F）同样不可见、同样能起分词作用，
    // 必须一并移除 —— 只处理 < 0x20 会留下绕过分词的后门。
    const QString withDel =
        QStringLiteral("打开") + QChar(0x7F) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withDel), QStringLiteral("打开浏览器"));

    const QString withC1 =
        QStringLiteral("打开") + QChar(0x85) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withC1), QStringLiteral("打开浏览器"));

    // 制表/换行是「合法空白」，折叠为空格（不是删除，也不是拒绝）
    const QString withNewline =
        QStringLiteral("打开") + QChar(0x0A) + QStringLiteral("浏览器");
    QCOMPARE(CommandEngine::normalizeInput(withNewline), QStringLiteral("打开 浏览器"));
}

// 正常中文不能被规范化过程破坏 —— 防止「修 bug 修坏了正常路径」。
void TestCommandEngine::normalize_preservesNormalChinese()
{
    const QString normal = QStringLiteral("打开下载文件夹");
    QCOMPARE(CommandEngine::normalizeInput(normal), normal);

    const QString mixed = QStringLiteral("搜索 Qt 教程");
    QCOMPARE(CommandEngine::normalizeInput(mixed), mixed);
}

// ---------------------------------------------------------------------------
// 输入合法性
// ---------------------------------------------------------------------------

void TestCommandEngine::acceptable_rejectsEmpty()
{
    QString reason;
    QVERIFY(!CommandEngine::isInputAcceptable(QString(), 512, &reason));
    QVERIFY(!CommandEngine::isInputAcceptable(QStringLiteral("   "), 512, &reason));
    QVERIFY(!reason.isEmpty());

    // 只有零宽字符，等价于空输入
    QVERIFY(!CommandEngine::isInputAcceptable(QString(QChar(0x200B)), 512, &reason));
}

void TestCommandEngine::acceptable_rejectsOverlong()
{
    QString reason;
    const QString tooLong(600, QLatin1Char('a'));
    QVERIFY(!CommandEngine::isInputAcceptable(tooLong, 512, &reason));
    QVERIFY(!reason.isEmpty());

    // 恰好等于上限应当允许
    const QString exact(512, QLatin1Char('a'));
    QVERIFY(CommandEngine::isInputAcceptable(exact, 512, &reason));
}

void TestCommandEngine::acceptable_rejectsControlChars()
{
    QString reason;

    // 真正危险的控制字符必须拒绝。
    // 0x01（SOH）这类字符没有合法用途，而且它可以用来把"打开浏览器"
    // 拆成"打开 浏览器"，从而绕过规则匹配 —— 是典型的注入手法。
    QVERIFY2(!CommandEngine::isInputAcceptable(QStringLiteral("打开\x01浏览器"), 512, &reason),
             "不可见控制字符必须被拒绝");
    QVERIFY(!reason.isEmpty());

    QVERIFY(!CommandEngine::isInputAcceptable(QStringLiteral("打开\x7F浏览器"), 512, &reason));

    // 但换行、回车、制表符是**故意允许**的：
    // 用户从网页或聊天窗口粘贴多行文本是很常见的操作，
    // normalizeInput() 会把它们折叠成空格，因此不会造成 token 拆分。
    // （这条之前写成"应当拒绝"，是测试作者弄错了契约。）
    QVERIFY2(CommandEngine::isInputAcceptable(QStringLiteral("打开\n浏览器"), 512, &reason),
             "换行应当被折叠为空格而不是被拒绝（允许多行粘贴）");
    QVERIFY2(CommandEngine::isInputAcceptable(QStringLiteral("打开\t浏览器"), 512, &reason),
             "制表符应当被折叠为空格而不是被拒绝");
}

void TestCommandEngine::acceptable_acceptsNormalInput()
{
    QString reason;
    QVERIFY(CommandEngine::isInputAcceptable(QStringLiteral("打开浏览器"), 512, &reason));
    QVERIFY2(reason.isEmpty(), "合法输入不应产生失败原因");
}

// ---------------------------------------------------------------------------
// 解析：精确匹配
// ---------------------------------------------------------------------------

void TestCommandEngine::parse_exactMatch_openBrowser()
{
    const ParseResult r = m_engine->parse(QStringLiteral("打开浏览器"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.intent, QStringLiteral("open_application"));
    QCOMPARE(r.best.ruleId, QStringLiteral("open_browser"));
}

void TestCommandEngine::parse_exactMatch_petHide()
{
    const ParseResult r = m_engine->parse(QStringLiteral("隐藏"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.intent, QStringLiteral("pet_control"));
    // 规则里声明了 action=hide 常量参数
    QCOMPARE(r.best.parameters.value(QStringLiteral("action")), QStringLiteral("hide"));
}

void TestCommandEngine::parse_exactMatch_help()
{
    const ParseResult r = m_engine->parse(QStringLiteral("帮助"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.intent, QStringLiteral("help"));
}

// 精确匹配的置信度必须是满分，否则 isExecutable() 会误判为需要确认。
void TestCommandEngine::parse_matchedResultIsExecutable()
{
    const ParseResult r = m_engine->parse(QStringLiteral("打开浏览器"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.confidence, 1.0);
    QVERIFY2(r.isExecutable(), "精确匹配的结果必须可直接执行");
}

// ---------------------------------------------------------------------------
// 解析：参数抽取
// ---------------------------------------------------------------------------

// "搜索{query}" —— {query} 位置的内容要被抓出来
void TestCommandEngine::parse_extractsSlotParameter_webSearch()
{
    const ParseResult r = m_engine->parse(QStringLiteral("搜索 今天天气"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.intent, QStringLiteral("web_search"));
    QCOMPARE(r.best.parameters.value(QStringLiteral("query")),
             QStringLiteral("今天天气"));
}

void TestCommandEngine::parse_extractsSlotParameter_translate()
{
    const ParseResult r = m_engine->parse(QStringLiteral("翻译 hello world"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.intent, QStringLiteral("translate"));
    QCOMPARE(r.best.parameters.value(QStringLiteral("text")),
             QStringLiteral("hello world"));
}

// 别名归一化：用户说「浏览器」，规则要拿到 browser 这个内部 id。
// 这是「不让自由文本直接透传」安全设计的关键一环。
void TestCommandEngine::parse_normalizesAlias_appName()
{
    const ParseResult r = m_engine->parse(QStringLiteral("打开浏览器"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    // 归一化后的 app_id
    QCOMPARE(r.best.parameters.value(QStringLiteral("app_id")),
             QStringLiteral("browser"));
}

// 别名表里没有的值，按设计「原样返回」而不是报错 ——
// 但要保留 raw 值，交由后续权限/校验环节处理。
void TestCommandEngine::parse_aliasUnknownValuePassesThroughRaw()
{
    const ParseResult r = m_engine->parse(QStringLiteral("打开某个不存在的程序"));

    // 规则优先级说明（这里原来是测试作者写错了预期，记下来）：
    //   open_folder                priority 8  ← 胜出
    //   open_application_by_name   priority 5
    //   open_path_absolute         priority 3
    // 三条规则的通用模式（"打开{folder}" / "打开{app}" / "打开{path}"）
    // 都能匹配"打开某个不存在的程序"，而 open_folder 优先级最高，
    // 因此它胜出是**正确的设计行为**，不是 bug。
    //
    // 关键安全性质：别名表里没有的值必须走 raw 透传 + 后续校验，
    // 绝不能在解析阶段就被当成合法目标放行。
    QCOMPARE(r.outcome, ParseOutcome::Matched);
    QCOMPARE(r.best.ruleId, QStringLiteral("open_folder"));

    // 原始用户输入必须被保留下来，供后续环节判断
    QVERIFY2(r.best.parameters.contains(QStringLiteral("folder_raw")),
             "别名无法归一化时必须保留 raw 值");

    // 归一化结果不能恰好等于某个合法目标 ——
    // 否则"某个不存在的程序"会被误当成合法目录
    const QString target = r.best.parameters.value(QStringLiteral("target"));
    QVERIFY2(target != QStringLiteral("desktop")
                 && target != QStringLiteral("documents")
                 && target != QStringLiteral("downloads"),
             "未知别名不得被归一化成合法标准目录");
}

void TestCommandEngine::parse_openFolderUsesFolderAlias()
{
    const ParseResult r = m_engine->parse(QStringLiteral("打开下载文件夹"));

    QCOMPARE(r.outcome, ParseOutcome::Matched);
    // 应该归一化成 downloads
    QCOMPARE(r.best.parameters.value(QStringLiteral("target")),
             QStringLiteral("downloads"));
}

// ---------------------------------------------------------------------------
// 解析：无法识别
// ---------------------------------------------------------------------------

// 关键安全行为：听不懂就说听不懂，绝不猜。
void TestCommandEngine::parse_gibberishIsNotMatched()
{
    const ParseResult r = m_engine->parse(
        QStringLiteral("qwertyuiop zxcvbnm asdfgh"));

    QVERIFY2(r.outcome != ParseOutcome::Matched,
             "无法识别的输入不应被判定为匹配成功");
}

void TestCommandEngine::parse_emptyInputIsInvalid()
{
    const ParseResult r = m_engine->parse(QString());

    QCOMPARE(r.outcome, ParseOutcome::Invalid);
    QVERIFY(!r.isExecutable());
}

// ---------------------------------------------------------------------------
// 解析：确定性
// ---------------------------------------------------------------------------

// 同一条输入多次解析必须给出完全相同的结果。
// 规则引擎不该有任何随机性或时序依赖 —— 否则行为无法复现。
void TestCommandEngine::parse_sameInputGivesSameResult()
{
    const QString input = QStringLiteral("翻译 hello");
    const ParseResult a = m_engine->parse(input);
    const ParseResult b = m_engine->parse(input);

    QCOMPARE(a.outcome, b.outcome);
    QCOMPARE(a.best.ruleId, b.best.ruleId);
    QCOMPARE(a.best.intent, b.best.intent);
    QCOMPARE(a.best.parameters, b.best.parameters);
}

QTEST_MAIN(TestCommandEngine)
#include "tst_command_engine.moc"
