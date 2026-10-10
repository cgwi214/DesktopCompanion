// ============================================================================
//  LanguageDetector.cpp — 多语言识别实现
//
//  需求依据：SRS V1.1 4.2
//
//  判定规则（可解释、可测试、无模型依赖）：
//    1. 若无有效字符 -> Auto，置信度 0
//    2. 若存在平假名或片假名 -> 日语（置信度随假名占比提升）
//    3. 若存在 CJK 汉字且无假名 -> 中文
//    4. 若几乎全是拉丁字母 -> 英语
//    5. 混合情况按主导字符类别判定，并降低置信度
//    6. 置信度低于阈值时返回 Auto，由上层请求用户确认（SRS 4.2）
// ============================================================================
#include "engine/LanguageDetector.h"

namespace dc {

namespace {

// Unicode 区间判定工具
inline bool isHan(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x4E00 && u <= 0x9FFF)      // CJK 统一汉字
        || (u >= 0x3400 && u <= 0x4DBF)      // 扩展 A
        || (u >= 0xF900 && u <= 0xFAFF)      // 兼容汉字
        || (u >= 0x20000 && u <= 0x2A6DF);   // 扩展 B（需代理对，单 QChar 判不到，保留）
}

inline bool isHiragana(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x3040 && u <= 0x309F)      // 平假名
        || (u >= 0x1B001 && u <= 0x1B11F);   // 古平假名变体（罕见）
}

inline bool isKatakana(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 0x30A0 && u <= 0x30FF)      // 片假名
        || (u >= 0x31F0 && u <= 0x31FF)      // 片假名扩展
        || (u >= 0xFF66 && u <= 0xFF9D);     // 半角片假名
}

inline bool isLatin(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'A' && u <= 'Z')
        || (u >= 'a' && u <= 'z')
        || (u >= 0x00C0 && u <= 0x024F)      // 拉丁扩展（带重音符号）
        || (u >= 0xFF21 && u <= 0xFF3A)      // 全角大写字母
        || (u >= 0xFF41 && u <= 0xFF5A);     // 全角小写字母
}

// 判断是否为「结构性字符」——标点、数字、空白。
// 这些字符不参与语言判定，也不应拉低置信度。
inline bool isStructural(QChar c)
{
    if (c.isSpace())
        return true;
    if (c.isDigit())
        return true;

    const ushort u = c.unicode();

    // ASCII 标点
    if (u < 0x80 && (u == '!' || u == '"' || u == '#' || u == '$' || u == '%'
                     || u == '&' || u == '\'' || u == '(' || u == ')' || u == '*'
                     || u == '+' || u == ',' || u == '-' || u == '.' || u == '/'
                     || u == ':' || u == ';' || u == '<' || u == '=' || u == '>'
                     || u == '?' || u == '@' || u == '[' || u == '\\' || u == ']'
                     || u == '^' || u == '_' || u == '`' || u == '{' || u == '|'
                     || u == '}' || u == '~')) {
        return true;
    }

    // CJK 标点：。、？！「」『』（）《》〈〉【】…—
    if ((u >= 0x3000 && u <= 0x303F)
        || (u >= 0xFF01 && u <= 0xFF0F)
        || (u >= 0xFF1A && u <= 0xFF20)
        || (u >= 0xFF3B && u <= 0xFF40)
        || (u >= 0xFF5B && u <= 0xFF65)) {
        return true;
    }

    return false;
}

} // namespace

// ===========================================================================
//  字符统计
// ===========================================================================
LanguageGuess LanguageDetector::analyze(const QString &text)
{
    LanguageGuess guess;

    for (const QChar &c : text) {
        if (isHiragana(c)) {
            ++guess.hiraganaCount;
        } else if (isKatakana(c)) {
            ++guess.katakanaCount;
        } else if (isHan(c)) {
            ++guess.hanCount;
        } else if (isLatin(c)) {
            ++guess.latinCount;
        } else if (isStructural(c)) {
            // 结构性字符不计入任何类别
            continue;
        } else {
            ++guess.otherCount;
        }
    }

    return guess;
}

// ===========================================================================
//  语言判定
// ===========================================================================
LanguageGuess LanguageDetector::detect(const QString &text, Language fallback)
{
    LanguageGuess guess = analyze(text);

    const int kana = guess.hiraganaCount + guess.katakanaCount;
    const int han = guess.hanCount;
    const int latin = guess.latinCount;
    const int other = guess.otherCount;

    const int meaningful = kana + han + latin + other;

    // -----------------------------------------------------------------------
    // 关于 fallback 的语义（这里曾经有一个 bug，记下来防复发）
    //
    // 本函数**保证返回一个可直接使用的具体语言**，绝不会返回 Language::Auto。
    // 当字符统计无法得出可靠结论时，返回调用方传入的 fallback。
    //
    // 早期实现在「无有效字符」「未支持的文字系统」两个分支里返回 Language::Auto，
    // 把回退这件事推给了调用方。但调用方 CommandEngine 拿到 Auto 后只是
    // 原样存进 result.detectedLanguage，并没有再做回退 —— 而
    // languageMatches() 把 Auto 当作「通配」，于是语言过滤被静默跳过，
    // 三语规则全部参与匹配。表现为「指定语言不生效」，很难察觉。
    //
    // 教训：一个叫 fallback 的参数，如果只是用来拼提示文案、而不真正改变
    // 返回值，那它就是个陷阱。要么真正生效，要么别叫这个名字。
    // 单元测试 tests/unit/tst_language_detect.cpp 覆盖了这条契约。
    // -----------------------------------------------------------------------

    // ---- 规则 1：无有效字符 ----
    if (meaningful == 0) {
        guess.language = fallback;
        guess.confidence = 0.0;
        guess.reason = QStringLiteral("输入中没有可识别的文字字符，"
                                      "使用你设置的语言：%1。")
                           .arg(languageDisplayName(fallback));
        return guess;
    }

    // ---- 规则 2：出现假名 -> 日语 ----
    //  假名是日语的强特征，在中日文本中都极少出现。
    if (kana > 0) {
        const double kanaRatio = static_cast<double>(kana) / meaningful;

        guess.language = Language::JaJP;

        // 置信度设计：
        //   假名占比 >= 30%  -> 高置信
        //   假名占比 >= 10%  -> 中等
        //   仅 1~2 个假名     -> 较低（可能是「日本語」这类词汇被误用，
        //                       也可能是中英文本中引用了一个日文词）
        if (kanaRatio >= 0.30) {
            guess.confidence = 0.95;
            guess.reason = QStringLiteral("检测到假名（%1 个，占比 %2%），判定为日语。")
                               .arg(kana).arg(static_cast<int>(kanaRatio * 100));
        } else if (kanaRatio >= 0.10) {
            guess.confidence = 0.82;
            guess.reason = QStringLiteral("检测到少量假名（%1 个），倾向判定为日语。")
                               .arg(kana);
        } else if (kana >= 2) {
            guess.confidence = 0.70;
            guess.reason = QStringLiteral("仅检测到 %1 个假名，可能是日语。").arg(kana);
        } else {
            guess.confidence = 0.55;
            guess.reason = QStringLiteral("仅检测到 1 个假名，语言判定把握不大，"
                                          "建议手动指定语言。");
        }
        return guess;
    }

    // ---- 规则 3：有汉字且无假名 -> 中文 ----
    if (han > 0) {
        const double hanRatio = static_cast<double>(han) / meaningful;

        guess.language = Language::ZhCN;

        if (hanRatio >= 0.50) {
            guess.confidence = 0.92;
            guess.reason = QStringLiteral("检测到汉字（%1 个，占比 %2%），"
                                          "且未检测到假名，判定为中文。")
                               .arg(han).arg(static_cast<int>(hanRatio * 100));
        } else if (hanRatio >= 0.20) {
            guess.confidence = 0.78;
            guess.reason = QStringLiteral("检测到部分汉字（%1 个），"
                                          "且未检测到假名，倾向判定为中文。").arg(han);
        } else {
            // 汉字很少、拉丁字母很多：可能是中英混排（如「打开 Chrome」）。
            // 这类输入在命令场景中很常见，判为中文更符合用户预期，
            // 但置信度要低一些。
            guess.confidence = 0.65;
            guess.reason = QStringLiteral("汉英混排（汉字 %1 个，拉丁字母 %2 个），"
                                          "按中文处理。").arg(han).arg(latin);
        }
        return guess;
    }

    // ---- 规则 4：纯拉丁 -> 英语 ----
    if (latin > 0 && other == 0) {
        const double latinRatio = static_cast<double>(latin) / meaningful;

        guess.language = Language::EnUS;

        if (latinRatio >= 0.95) {
            guess.confidence = 0.93;
            guess.reason = QStringLiteral("输入几乎全部为拉丁字母，判定为英语。");
        } else {
            guess.confidence = 0.75;
            guess.reason = QStringLiteral("输入以拉丁字母为主，倾向判定为英语。");
        }
        return guess;
    }

    // ---- 规则 5：混合且无明确主导 ----
    //  到这里说明：没有假名、没有汉字、不是纯拉丁（含其他文字系统，
    //  如韩文、西里尔文、阿拉伯文等）。本版本不支持这些语言，
    //  因此回退到调用方配置的语言（**不是** Language::Auto —— 理由见函数开头）。
    guess.language = fallback;
    guess.confidence = 0.30;
    guess.reason = QStringLiteral(
        "无法可靠识别输入语言（可能包含本版本未支持的文字系统），"
        "将使用你设置的语言：%1。")
        .arg(languageDisplayName(fallback));
    return guess;
}

// ===========================================================================
bool LanguageDetector::requiresIme(Language language)
{
    switch (language) {
    case Language::ZhCN:
    case Language::JaJP:
        return true;
    case Language::EnUS:
    case Language::Auto:
        break;
    }
    return false;
}

} // namespace dc
