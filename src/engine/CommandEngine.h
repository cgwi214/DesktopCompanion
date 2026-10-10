// ============================================================================
//  CommandEngine.h — 命令解析引擎
//
//  需求依据：SRS V1.1 第 4.6 节（命令引擎）
//    「命令引擎采用规则、模板、关键词、有限状态机和配置化语法，
//      不依赖生成式模型。」
//
//    要求：
//      - 支持命令匹配、参数提取、别名和优先级
//      - 对冲突规则提供配置校验
//      - 对低置信度或多义输入请求澄清
//      - 所有命令执行前进行权限和参数校验
//      - 用户自定义命令不得直接执行任意拼接的 shell 字符串
//
//  SRS 3.4 给出的接口：
//      def parse(self, text, language="auto"):
//          """解析输入，返回意图、参数、置信度和澄清需求。"""
//
//  实现架构（对应 SRS 提到的四种技术）：
//    ┌─────────────────────────────────────────────────┐
//    │ 1. 输入规范化（全角转半角、去噪、长度校验）      │  ← 有限状态预处理
//    ├─────────────────────────────────────────────────┤
//    │ 2. 语言判定（LanguageDetector）                  │  ← 规则 + 字符范围
//    ├─────────────────────────────────────────────────┤
//    │ 3. 精确模式匹配（编译后正则）                    │  ← 模板匹配
//    ├─────────────────────────────────────────────────┤
//    │ 4. 关键词评分（当精确匹配失败时的兜底）          │  ← 关键词
//    ├─────────────────────────────────────────────────┤
//    │ 5. 参数提取 + 别名归一化 + 值校验                │  ← 配置化语法
//    ├─────────────────────────────────────────────────┤
//    │ 6. 优先级排序 / 歧义判定 / 澄清提示生成          │
//    └─────────────────────────────────────────────────┘
// ============================================================================
#pragma once

#include "engine/CommandRepository.h"
#include "model/ParseResult.h"
#include "model/Types.h"

#include <QObject>
#include <QString>
#include <QMap>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 引擎配置
// ---------------------------------------------------------------------------
struct CommandEngineConfig {
    // 精确匹配成功的置信度；模板命中视为完全可信
    double exactMatchConfidence = 1.0;

    // 关键词模糊匹配的基础置信度（每个命中关键词加分）
    double keywordBaseConfidence = 0.45;
    double keywordHitBonus = 0.12;

    // 低于此置信度视为 LowConfidence，需要向用户确认（SRS 4.6）
    double minConfidence = 0.60;

    // 输入长度上限（SRS 7.3：所有外部输入均需校验）
    int maxInputLength = 512;

    // 是否启用关键词兜底匹配
    bool enableKeywordFallback = true;
};

// ---------------------------------------------------------------------------
// CommandEngine
// ---------------------------------------------------------------------------
class CommandEngine : public QObject
{
    Q_OBJECT

public:
    CommandEngine(CommandRepository *repository, QObject *parent = nullptr);
    ~CommandEngine() override;

    void setConfig(const CommandEngineConfig &config);
    CommandEngineConfig config() const;

    // -----------------------------------------------------------------------
    // 主入口：解析输入
    //
    //  SRS 3.4 契约：
    //    text     —— 原始输入（文本输入或语音识别结果）
    //    language —— 指定语言；Auto 表示自动检测
    //
    //  返回 ParseResult，包含：
    //    - outcome（Matched / Ambiguous / NoMatch / LowConfidence / Invalid）
    //    - 最佳候选（含意图、参数、置信度、风险等级）
    //    - 歧义时的候选列表
    //    - 面向用户的澄清问句
    //
    //  重要：本函数只解析，不执行任何操作。
    //        语音识别结果必须进入本函数而不得直接执行（SRS 3.2）。
    // -----------------------------------------------------------------------
    ParseResult parse(const QString &text, Language language = Language::Auto) const;

    // -----------------------------------------------------------------------
    // 输入规范化（公开以便单元测试覆盖）
    //
    //  处理内容：
    //    1. 去除首尾空白、折叠内部连续空白
    //    2. 全角字母/数字/标点转半角（用户常在中英切换时混用全角）
    //    3. 去除零宽字符与不可见控制字符（防注入与混淆）
    //    4. 中文标点归一（，。？！ -> , . ? !）以便规则只用一套写法
    //    5. 长度校验
    // -----------------------------------------------------------------------
    static QString normalizeInput(const QString &text);

    // 输入合法性检查（长度、控制字符）
    static bool isInputAcceptable(const QString &text, int maxLength,
                                  QString *reason = nullptr);

signals:
    // 解析完成（用于日志与调试界面）
    void parsed(const dc::ParseResult &result);

private:
    // 精确模式匹配：返回所有命中的候选
    QVector<CommandCandidate> matchByPattern(const QString &normalized,
                                            Language language) const;

    // 关键词兜底匹配
    QVector<CommandCandidate> matchByKeyword(const QString &normalized,
                                            Language language) const;

    // 从命中结果中提取参数（含别名归一化与正则校验）
    // 返回 false 表示参数校验失败（此时应在候选中记录失败原因）
    bool extractParameters(const CommandRule &rule, int patternIndex,
                          const QRegularExpressionMatch &match,
                          QMap<QString, QString> *out,
                          QString *failureReason) const;

    // 生成澄清问句（SRS 4.2 TXT-004：不得猜测执行高风险操作）
    static QString buildClarificationPrompt(const QVector<CommandCandidate> &candidates,
                                           Language language);

    // 语言匹配度：规则语言与输入语言是否相容
    static bool languageMatches(Language ruleLanguage, Language inputLanguage);

    CommandRepository *m_repository = nullptr;
    CommandEngineConfig m_config;
};

} // namespace dc
