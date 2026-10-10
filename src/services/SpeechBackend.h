#pragma once

// ============================================================================
//  SpeechBackend.h —— 语音能力的可替换后端接口（SRS 4.10）
//
//  需求原文（SRS 4.10 语音与录音）：
//    "语音识别与语音合成应设计为可替换后端；
//     未配置后端时相关入口置灰并说明原因。"
//
//  以及 SRS 8.3（技术路线）：
//    "语音能力优先使用操作系统自带能力，避免强制引入大型模型。"
//
//  设计立场（这一点很重要）：
//    本项目不内置任何语音识别模型。原因：
//      1. 现代语音识别模型动辄数百 MB，与"轻量、低占用"的核心要求冲突；
//      2. 引入模型就意味着引入深度学习依赖，而 SRS 明确要求不使用；
//      3. 操作系统自带的语音能力（Windows SAPI）在旧系统上也有，
//         且零额外体积。
//    因此本模块的定位是「接口 + 系统能力适配」，而不是「实现一个识别器」。
//
//  后端能力分级（SpeechBackendKind）：
//    - Full          : 识别与合成都可用
//    - SynthesisOnly : 只能合成（朗读），这是最常见的情况
//    - RecognitionOnly: 只能识别
//    - RuleBasedOnly : 只能做规则匹配（例如用固定词表匹配少数指令），
//                      不是真正的语音识别，但聊胜于无，且必须如实告知用户
//    - None          : 不可用
//
//  为什么要有 RuleBasedOnly 这一档：
//    在没有真正识别能力的系统上，如果直接置灰语音入口，用户会觉得功能缺失。
//    如果伪装成能识别，则是欺骗。RuleBasedOnly 是一个诚实的中间档：
//    明确告诉用户"只能识别少数几个固定指令"。
//
//  注意与 model/Types.h 里 SpeechCapability 的区别：
//    SpeechCapability 是「描述后端能力的结构体」（含 available 标志与原因），
//    SpeechBackendKind 是「后端自身能力的分级枚举」。两者用途不同，
//    转换由 beKindToCapability() 完成。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QHash>
#include <QList>

#include "model/Types.h"

namespace dc {

// ---------------------------------------------------------------------------
// 后端能力分级
// ---------------------------------------------------------------------------
enum class SpeechBackendKind {
    None = 0,
    RuleBasedOnly,
    RecognitionOnly,
    SynthesisOnly,
    Full
};

QString speechBackendKindToString(SpeechBackendKind kind);
SpeechBackendKind speechBackendKindFromString(const QString &text);

// 把分级枚举转换成 model 层的能力结构体（供 SystemInfo / 界面使用）
SpeechCapability toModelCapability(SpeechBackendKind kind,
                                   const QString &backendId = QString(),
                                   const QString &backendDisplayName = QString(),
                                   const QString &unavailableReason = QString());

// ---------------------------------------------------------------------------
// 识别请求
// ---------------------------------------------------------------------------
struct RecognitionRequest {
    // 音频数据（PCM 16-bit little-endian 单声道，采样率见 sampleRate）
    QByteArray pcmData;
    int sampleRate = 16000;
    int channels = 1;
    int bitsPerSample = 16;

    // 期望语言（空 = 自动）
    QString language;

    // 超时（毫秒）
    int timeoutMs = 15000;
};

// ---------------------------------------------------------------------------
// 识别结果
// ---------------------------------------------------------------------------
struct RecognitionResult {
    bool ok = false;
    QString errorCode;
    QString message;

    // 识别出的文本。空 + ok=true 表示"听到了但没有识别出内容"，
    // 这与"识别失败"是两件事，界面上的表现也不同。
    QString text;

    // 置信度 0.0 ~ 1.0。低于阈值时界面应当请用户确认而不是直接执行命令。
    // -1 表示后端不提供置信度（不要假装知道）。
    double confidence = -1.0;

    // 是否来自规则匹配（RuleBasedOnly 后端）
    bool ruleBased = false;

    qint64 elapsedMs = 0;
};

// ---------------------------------------------------------------------------
// 合成请求
// ---------------------------------------------------------------------------
struct SynthesisRequest {
    QString text;
    QString language;
    QString voiceId;         // 空 = 默认音色
    int rate = 0;            // -10 ~ 10，0 为正常语速
    int volume = 100;        // 0 ~ 100
};

// ---------------------------------------------------------------------------
// 合成结果
// ---------------------------------------------------------------------------
struct SynthesisResult {
    bool ok = false;
    QString errorCode;
    QString message;

    // 合成出的音频（WAV 格式）。为空表示后端直接播放而不回传数据。
    QByteArray audioData;
    bool playedDirectly = false;
    qint64 durationMs = 0;
};

// ---------------------------------------------------------------------------
// 音色信息
// ---------------------------------------------------------------------------
struct VoiceInfo {
    QString id;
    QString displayName;
    QString language;
    QString gender;          // "male" / "female" / "" 未知
};

// ============================================================================
// SpeechBackend —— 语音后端接口
// ============================================================================
class SpeechBackend : public QObject
{
    Q_OBJECT

public:
    explicit SpeechBackend(QObject *parent = nullptr) : QObject(parent) {}
    ~SpeechBackend() override = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;

    // 这个后端提供哪些能力
    virtual SpeechBackendKind kind() const = 0;

    // 为什么不可用（不可用时给用户看的原因）
    virtual QString unavailableReason() const { return QString(); }

    bool canRecognize() const
    {
        const SpeechBackendKind k = kind();
        return k == SpeechBackendKind::Full || k == SpeechBackendKind::RecognitionOnly
               || k == SpeechBackendKind::RuleBasedOnly;
    }
    bool canSynthesize() const
    {
        const SpeechBackendKind k = kind();
        return k == SpeechBackendKind::Full || k == SpeechBackendKind::SynthesisOnly;
    }

    // 可用音色
    virtual QList<VoiceInfo> availableVoices() const { return QList<VoiceInfo>(); }

    // 支持的识别语言
    virtual QStringList supportedLanguages() const { return QStringList(); }

    // ---- 识别 ----
    virtual void recognize(const RecognitionRequest &request) = 0;
    virtual void cancelRecognition() {}

    // ---- 合成 ----
    virtual void synthesize(const SynthesisRequest &request) = 0;
    virtual void cancelSynthesis() {}
    virtual void stopPlayback() {}

    // 是否正在朗读
    virtual bool isSpeaking() const { return false; }

signals:
    void recognitionFinished(const dc::RecognitionResult &result);
    void synthesisFinished(const dc::SynthesisResult &result);

    // 朗读开始/结束（用于桌宠切换状态）
    void speakingChanged(bool speaking);
};

// ============================================================================
// RuleBasedSpeechBackend —— 规则匹配兜底后端
//
//  在没有真正语音识别能力的系统上提供"能听懂几个固定指令"的能力。
//
//  诚实性原则：
//    1. 它的 capability() 返回 RuleBasedOnly，界面必须如实展示
//       "仅支持少量固定指令"，不能包装成"语音识别"。
//    2. 识别结果的 confidence 固定给一个偏低的值（0.5），
//       并且 ruleBased = true，让上层知道这不适合直接执行高风险命令。
//    3. 它不接受任意音频输入 —— 真正的音频转文本不在这里做，
//       而是由 RecorderService + 外部工具完成。这里只做"文本到指令"的匹配。
//       如果用户把任意文本喂进来，它能做的也只是查词表。
// ============================================================================
class RuleBasedSpeechBackend : public SpeechBackend
{
    Q_OBJECT

public:
    explicit RuleBasedSpeechBackend(QObject *parent = nullptr);
    ~RuleBasedSpeechBackend() override;

    QString id() const override;
    QString displayName() const override;
    SpeechBackendKind kind() const override;

    QStringList supportedLanguages() const override { return { QStringLiteral("zh"),
                                                              QStringLiteral("en") }; }

    void recognize(const RecognitionRequest &request) override;
    void synthesize(const SynthesisRequest &request) override;

    // 注册一个"听起来的词" -> "规范指令词" 的映射
    void addPhrase(const QString &spokenForm, const QString &canonical);

    // 已注册的短语数量
    int phraseCount() const;

    // 内置短语表
    void loadBuiltinPhrases();

private:
    QHash<QString, QString> m_phrases;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::RecognitionResult)
Q_DECLARE_METATYPE(dc::SynthesisResult)
