#pragma once

// ============================================================================
//  SpeechService —— 语音能力的统一入口（SRS 4.10）
//
//  需求原文（SRS 4.10 语音与录音）：
//    - 语音识别与语音合成应设计为可替换后端；
//    - 未配置后端时相关入口置灰并说明原因（SRS 6.2）；
//    - 语音输入应支持"按住说话"，松开后识别；
//    - 识别置信度不足时应请用户确认，而不是直接执行命令。
//
//  职责边界：
//    SpeechService 管理「后端的选择与降级」，不实现任何语音算法。
//    真正的语音处理在 SpeechBackend 的具体实现里（如 SapiSpeechBackend）。
//
//  后端选择策略（按优先级尝试）：
//    1. 用户配置里指定的后端
//    2. SAPI（Windows 系统自带，能力最完整）
//    3. 规则匹配（永远可用的兜底，但能力有限）
//
//  这个策略的实现要点是「逐级降级且如实告知」：
//    如果最终落到规则匹配，界面必须显示"仅支持少量固定指令"，
//    而不是显示"语音已就绪"。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>

#include "model/Types.h"
#include "services/SpeechBackend.h"

namespace dc {

// ---------------------------------------------------------------------------
// 语音服务配置
// ---------------------------------------------------------------------------
struct SpeechServiceConfig {
    // 用户指定的后端 id（空 = 自动选择）
    QString preferredBackendId;

    QString language = QStringLiteral("zh");

    // 朗读相关
    bool synthesisEnabled = true;
    QString voiceId;              // 空 = 后端默认
    int rate = 0;                 // -10 ~ 10
    int volume = 100;             // 0 ~ 100

    // 识别相关
    bool recognitionEnabled = false;

    // 基于规则的后端是否可用于执行命令（默认 false）。
    //
    //  为什么默认 false：
    //    规则匹配后端只有 0.5 的置信度，它命中一个关键词就返回结果。
    //    如果默认允许它触发命令，那"打开"这两个字出现在任何句子里
    //    都可能被当成指令执行 —— 这很危险。
    //    默认只把它当"候选建议"展示给用户确认。
    bool allowRuleBasedCommandExecution = false;

    // 置信度低于此值时请用户确认（SRS 4.10）
    double confidenceThreshold = 0.7;
};

// ---------------------------------------------------------------------------
// 服务状态（供界面展示）
// ---------------------------------------------------------------------------
struct SpeechServiceStatus {
    bool backendReady = false;
    SpeechBackendKind kind = SpeechBackendKind::None;
    QString activeBackendId;
    QString activeBackendName;

    bool canRecognize = false;
    bool canSynthesize = false;

    // 不可用或能力受限的原因（必须展示给用户）
    QString limitation;

    // 已注册的后端数量
    int backendCount = 0;
};

// ============================================================================
// SpeechService
// ============================================================================
class SpeechService : public QObject
{
    Q_OBJECT

public:
    explicit SpeechService(QObject *parent = nullptr);
    ~SpeechService() override;

    // ---- 初始化 ----
    // 注册所有可用的后端并完成能力探测。返回是否有任何可用后端。
    bool initialize(const SpeechServiceConfig &config = SpeechServiceConfig{});
    void applyConfig(const SpeechServiceConfig &config);

    SpeechServiceConfig config() const;
    SpeechServiceStatus status() const;

    // ---- 后端管理 ----
    void registerBackend(SpeechBackend *backend);
    QList<SpeechBackend *> backends() const;
    SpeechBackend *backend(const QString &id) const;

    // 当前使用的后端
    SpeechBackend *activeBackend() const;
    QString activeBackendId() const;

    // 按优先级自动选择后端。返回选中的后端 id（空 = 一个都没有）。
    QString selectBackend(const QString &preferredId = QString());

    // ---- 能力查询（界面用这些决定是否置灰入口）----
    bool canRecognize() const;
    bool canSynthesize() const;

    // 不可用原因。可用的返回空字符串。
    QString recognitionLimitation() const;
    QString synthesisLimitation() const;

    // ---- 朗读（合成）----

    // 异步朗读一段文本。返回 requestId。
    QString speak(const QString &text, const QString &language = QString());

    // 停止朗读
    void stopSpeaking();
    bool isSpeaking() const;

    // ---- 识别 ----

    // 发起一次识别（按住说话松开后调用）。
    // 返回 requestId。结果通过 recognitionFinished 信号返回。
    QString listen(const QByteArray &pcmData,
                   int sampleRate = 16000,
                   const QString &language = QString());

    // 取消进行中的识别
    void cancelListening();

    // 识别结果是否需要用户确认（置信度不足 或 来自规则匹配后端）
    bool needsConfirmation(const RecognitionResult &result) const;

    // ---- 音色 ----
    QList<VoiceInfo> availableVoices() const;
    bool setVoice(const QString &voiceId);

    // ---- 诊断 ----
    // 把后端情况拼成一段可读的说明，展示在设置页的"语音"区域。
    // 这是 SRS 6.2「能力探测结果控制功能入口」在界面上的落点。
    QString diagnosticReport() const;

signals:
    // 朗读开始/结束
    void speakingChanged(bool speaking);

    // 朗读完成
    void speechFinished(const QString &requestId, bool ok, const QString &message);

    // 识别完成
    void recognitionFinished(const QString &requestId, const dc::RecognitionResult &result);

    // 后端被切换（含自动降级）。界面应当据此刷新"能力说明"。
    void backendChanged(const QString &backendId, const dc::SpeechServiceStatus &status);

    // 能力受限提示（例如降级到规则匹配时）
    void limitationChanged(const QString &limitation);

private:
    void recomputeStatus();

    SpeechServiceConfig m_config;
    SpeechServiceStatus m_status;

    QHash<QString, SpeechBackend *> m_backends;
    QString m_activeBackendId;

    QString m_pendingSpeakId;
    QString m_pendingRecognitionId;
    quint64 m_counter = 0;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::SpeechServiceStatus)
