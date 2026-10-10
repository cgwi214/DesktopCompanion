// ============================================================================
//  LanguageDetector.h — 多语言识别（规则 + 字符范围 + 有限状态判定）
//
//  需求依据：SRS V1.1 第 4.2 节
//    TXT-001：支持中文、英语和日语输入
//    语言识别可先使用规则、字符范围和用户配置进行判断；
//    无法可靠识别时，允许用户手动指定语言。
//
//  设计约束（SRS 1.3 第 7 条）：
//    不使用生成式 AI / 深度学习模型，因此这里采用：
//      1. 字符区间统计（CJK 统一汉字 / 平假名 / 片假名 / 拉丁字母）
//      2. 假名存在性判定（日语的强特征：平假名/片假名只在日语中出现）
//      3. 规则优先级与置信度阈值
//
//  为什么假名是关键：
//    日语大量使用汉字，仅凭汉字无法区分中日。但只要出现假名，
//    几乎可以确定是日语。反过来，纯汉字文本判为中文（简体优先）。
//    这个规则简单、可解释，也便于单元测试覆盖（SRS 7.4）。
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QString>

namespace dc {

struct LanguageGuess {
    Language language = Language::Auto;
    double confidence = 0.0;      // 0.0 ~ 1.0
    QString reason;               // 判定依据（可解释性要求，SRS 1.3 第 5 条）

    // 各类字符的统计结果，便于调试与测试断言
    int hanCount = 0;
    int hiraganaCount = 0;
    int katakanaCount = 0;
    int latinCount = 0;
    int otherCount = 0;
};

class LanguageDetector
{
public:
    // 主入口：根据输入文本猜测语言
    //
    //  契约：**保证返回一个具体语言（ZhCN / EnUS / JaJP），绝不返回 Auto**。
    //        无法可靠判定时返回 fallback。fallback 通常来自用户配置，
    //        调用方应传一个具体语言而不是 Auto。
    //
    //  为什么强制不返回 Auto：
    //    Auto 在 languageMatches() 里是「通配」语义。如果本函数返回 Auto，
    //    上层把 Auto 存进 result.detectedLanguage 后，语言过滤就被静默跳过，
    //    三语规则会全部参与匹配 —— 表现为「用户指定的语言不生效」，
    //    而且完全没有任何报错。把回退收在本函数内部，契约更简单，也不易误用。
    //
    //  注：analyze() 是纯统计（**会**返回 Auto 表示「无字符」），
    //      detect() 才做判定并保证回退。两者语义不同，不要混用。
    static LanguageGuess detect(const QString &text, Language fallback = Language::ZhCN);

    // 仅统计字符构成（供测试与诊断使用）
    //  注意：本函数不做语言判定，无字符时 language 字段为 Auto。
    static LanguageGuess analyze(const QString &text);

    // 该语言是否需要 CJK 输入法提示
    static bool requiresIme(Language language);
};

} // namespace dc
