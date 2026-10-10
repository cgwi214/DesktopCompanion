// ============================================================================
//  tst_language_detect.cpp — 多语言识别单元测试
//
//  需求依据：SRS V1.1 4.2 TXT-001（支持中文、英语和日语输入）
//
//  设计要验证的核心判据（见 LanguageDetector.h 的说明）：
//    - 出现假名（平假名/片假名）=> 几乎可以确定是日语
//    - 纯汉字 => 判为中文（简体优先）
//    - 纯拉丁字母 => 英语
//
//  这个规则简单、可解释，也正是它能被单元测试完整覆盖的原因。
//  （对比：如果用模型，测试就只能验「大致差不多」，无法精确定断言。）
// ============================================================================
#include <QtTest/QtTest>

#include "engine/LanguageDetector.h"
#include "model/Types.h"

using namespace dc;

class TestLanguageDetect : public QObject
{
    Q_OBJECT

private slots:
    // ---- 字符统计 ----
    void analyze_countsChineseCharacters();
    void analyze_countsLatinCharacters();
    void analyze_countsHiragana();
    void analyze_countsKatakana();

    // ---- 语言判定 ----
    void detect_pureChineseIsZhCN();
    void detect_pureEnglishIsEnUS();
    void detect_hiraganaIsJapanese();
    void detect_katakanaIsJapanese();
    void detect_kanjiWithHiraganaIsJapanese();

    // ---- 边界 ----
    void detect_emptyFallsBackToDefault();
    void detect_confidenceIsInRange();
    void detect_providesReason();

    // ---- 一致性 ----
    void detect_sameInputSameResult();
};

// ---------------------------------------------------------------------------
// 字符统计
// ---------------------------------------------------------------------------

void TestLanguageDetect::analyze_countsChineseCharacters()
{
    const LanguageGuess g = LanguageDetector::analyze(QStringLiteral("打开浏览器"));

    QCOMPARE(g.hanCount, 5);
    QCOMPARE(g.hiraganaCount, 0);
    QCOMPARE(g.katakanaCount, 0);
    QCOMPARE(g.latinCount, 0);
}

void TestLanguageDetect::analyze_countsLatinCharacters()
{
    const LanguageGuess g = LanguageDetector::analyze(QStringLiteral("hello"));

    QCOMPARE(g.latinCount, 5);
    QCOMPARE(g.hanCount, 0);
}

void TestLanguageDetect::analyze_countsHiragana()
{
    // 「こんにちは」全是平假名（5 个字符）
    const LanguageGuess g = LanguageDetector::analyze(QStringLiteral("こんにちは"));

    QCOMPARE(g.hiraganaCount, 5);
    QCOMPARE(g.katakanaCount, 0);
}

void TestLanguageDetect::analyze_countsKatakana()
{
    // 「コンピュータ」是片假名
    const LanguageGuess g = LanguageDetector::analyze(QStringLiteral("コンピュータ"));

    QVERIFY2(g.katakanaCount > 0, "未统计到片假名");
    QCOMPARE(g.hiraganaCount, 0);
}

// ---------------------------------------------------------------------------
// 语言判定
// ---------------------------------------------------------------------------

void TestLanguageDetect::detect_pureChineseIsZhCN()
{
    const LanguageGuess g =
        LanguageDetector::detect(QStringLiteral("打开下载文件夹"));

    QCOMPARE(g.language, Language::ZhCN);
    QVERIFY(g.confidence > 0.0);
}

void TestLanguageDetect::detect_pureEnglishIsEnUS()
{
    const LanguageGuess g =
        LanguageDetector::detect(QStringLiteral("open the browser"));

    QCOMPARE(g.language, Language::EnUS);
    QVERIFY(g.confidence > 0.0);
}

// 假名是日语的强特征 —— 只要出现，就基本可以确定。
void TestLanguageDetect::detect_hiraganaIsJapanese()
{
    const LanguageGuess g = LanguageDetector::detect(QStringLiteral("こんにちは"));

    QCOMPARE(g.language, Language::JaJP);
    QVERIFY(g.confidence > 0.0);
}

void TestLanguageDetect::detect_katakanaIsJapanese()
{
    const LanguageGuess g = LanguageDetector::detect(QStringLiteral("ブラウザを開く"));

    QCOMPARE(g.language, Language::JaJP);
}

// 关键用例：汉字 + 假名 = 日语。
// 如果实现只看汉字数量，就会误判成中文 —— 这是最常见的错误。
void TestLanguageDetect::detect_kanjiWithHiraganaIsJapanese()
{
    // 「設定を開く」含汉字「設定」+ 平假名「を」「く」
    const LanguageGuess g = LanguageDetector::detect(QStringLiteral("設定を開く"));

    QCOMPARE(g.language, Language::JaJP);
    QVERIFY2(g.hanCount > 0, "该用例应同时含汉字，以验证不会误判为中文");
    QVERIFY(g.hiraganaCount > 0);
}

// ---------------------------------------------------------------------------
// 边界
// ---------------------------------------------------------------------------

// 空输入无法判定，应回退到调用方给的默认语言。
void TestLanguageDetect::detect_emptyFallsBackToDefault()
{
    const LanguageGuess g = LanguageDetector::detect(QString(), Language::EnUS);
    QCOMPARE(g.language, Language::EnUS);

    const LanguageGuess g2 = LanguageDetector::detect(QString(), Language::ZhCN);
    QCOMPARE(g2.language, Language::ZhCN);
}

void TestLanguageDetect::detect_confidenceIsInRange()
{
    const QStringList samples = {
        QStringLiteral("打开浏览器"),
        QStringLiteral("open browser"),
        QStringLiteral("ブラウザを開く"),
        QStringLiteral("こんにちは"),
        QStringLiteral("123456"),
        QStringLiteral("!@#$%"),
    };

    for (const QString &s : samples) {
        const LanguageGuess g = LanguageDetector::detect(s);
        QVERIFY2(g.confidence >= 0.0 && g.confidence <= 1.0,
                 qPrintable(QStringLiteral("置信度越界：%1 对输入「%2」")
                                .arg(g.confidence)
                                .arg(s)));
    }
}

// SRS 1.3 第 5 条要求可解释性：判定必须给出依据。
void TestLanguageDetect::detect_providesReason()
{
    const LanguageGuess g = LanguageDetector::detect(QStringLiteral("打开浏览器"));
    QVERIFY2(!g.reason.isEmpty(), "语言判定必须给出依据（可解释性要求）");
}

// ---------------------------------------------------------------------------
// 一致性
// ---------------------------------------------------------------------------

void TestLanguageDetect::detect_sameInputSameResult()
{
    const QString input = QStringLiteral("設定を開く");

    const LanguageGuess a = LanguageDetector::detect(input);
    const LanguageGuess b = LanguageDetector::detect(input);

    QCOMPARE(a.language, b.language);
    QCOMPARE(a.confidence, b.confidence);
    QCOMPARE(a.hanCount, b.hanCount);
    QCOMPARE(a.hiraganaCount, b.hiraganaCount);
}

QTEST_MAIN(TestLanguageDetect)
#include "tst_language_detect.moc"
