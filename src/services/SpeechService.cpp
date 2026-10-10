#include "SpeechService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "services/SapiSpeechBackend.h"

#include <QDateTime>
#include <QPointer>
#include <QTimer>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// 构造
// ============================================================================
SpeechService::SpeechService(QObject *parent)
    : QObject(parent)
{
}

SpeechService::~SpeechService() = default;

// ============================================================================
// 初始化
// ============================================================================
bool SpeechService::initialize(const SpeechServiceConfig &config)
{
    m_config = config;

    // 注册规则匹配后端 —— 它总是可用的兜底，所以先注册
    if (!m_backends.contains(QStringLiteral("rule_based"))) {
        auto *ruleBased = new RuleBasedSpeechBackend(this);
        registerBackend(ruleBased);
    }

    // 注册 SAPI 后端（Windows）并做一次真实的能力探测
    if (!m_backends.contains(QStringLiteral("sapi"))) {
        auto *sapi = new SapiSpeechBackend(this);
        const bool ok = sapi->initialize();
        // 即使探测失败也注册它 —— 失败本身是有价值的信息
        // （界面能显示"本系统不具备语音能力"，而不是"未知"）。
        registerBackend(sapi);
        if (!ok) {
            DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端探测结果为不可用：%1")
                        .arg(sapi->unavailableReason()));
        }
    }

    const QString selected = selectBackend(m_config.preferredBackendId);
    recomputeStatus();

    DC_INFO(LogCat::Speech, QStringLiteral("语音服务初始化完成：后端=%1 能力=%2")
                .arg(selected.isEmpty() ? QStringLiteral("(无)") : selected,
                     speechBackendKindToString(m_status.kind)));

    return m_status.backendReady;
}

void SpeechService::applyConfig(const SpeechServiceConfig &config)
{
    const QString previousBackend = m_activeBackendId;
    m_config = config;

    // 配置变化可能需要重新选后端（比如用户改了 preferredBackendId）
    selectBackend(m_config.preferredBackendId);
    recomputeStatus();

    // 应用朗读参数到当前后端
    SpeechBackend *active = activeBackend();
    if (active) {
        if (auto *sapi = qobject_cast<SapiSpeechBackend *>(active)) {
            sapi->setRate(m_config.rate);
            sapi->setVolume(m_config.volume);
            if (!m_config.voiceId.isEmpty()) {
                sapi->setVoiceById(m_config.voiceId);
            }
        }
    }

    if (previousBackend != m_activeBackendId) {
        emit backendChanged(m_activeBackendId, m_status);
    }
}

SpeechServiceConfig SpeechService::config() const
{
    return m_config;
}

SpeechServiceStatus SpeechService::status() const
{
    return m_status;
}

// ============================================================================
// 后端管理
// ============================================================================
void SpeechService::registerBackend(SpeechBackend *backend)
{
    if (!backend) {
        return;
    }
    const QString id = backend->id();
    if (id.isEmpty()) {
        DC_WARN(LogCat::Speech, QStringLiteral("语音后端 id 为空，拒绝注册"));
        return;
    }

    if (m_backends.contains(id)) {
        SpeechBackend *old = m_backends.value(id);
        old->disconnect(this);
        if (old->parent() == this) {
            old->deleteLater();
        }
    }

    m_backends.insert(id, backend);
    if (!backend->parent()) {
        backend->setParent(this);
    }

    // 统一转接信号
    const QPointer<SpeechBackend> safe(backend);

    connect(backend, &SpeechBackend::synthesisFinished, this,
            [this, safe](const SynthesisResult &result) {
                if (!safe) {
                    return;
                }
                const QString requestId = m_pendingSpeakId;
                m_pendingSpeakId.clear();

                if (result.ok) {
                    emit speechFinished(requestId, true, QString());
                } else {
                    emit speechFinished(requestId, false, result.message);
                }
            });

    connect(backend, &SpeechBackend::recognitionFinished, this,
            [this, safe](const RecognitionResult &result) {
                if (!safe) {
                    return;
                }
                const QString requestId = m_pendingRecognitionId;
                m_pendingRecognitionId.clear();
                emit recognitionFinished(requestId, result);
            });

    connect(backend, &SpeechBackend::speakingChanged, this,
            [this](bool speaking) {
                emit speakingChanged(speaking);
                recomputeStatus();
            });

    DC_INFO(LogCat::Speech, QStringLiteral("已注册语音后端：%1（%2，能力=%3）")
                .arg(id, backend->displayName(),
                     speechBackendKindToString(backend->kind())));
}

QList<SpeechBackend *> SpeechService::backends() const
{
    return m_backends.values();
}

SpeechBackend *SpeechService::backend(const QString &id) const
{
    return m_backends.value(id, nullptr);
}

SpeechBackend *SpeechService::activeBackend() const
{
    return m_backends.value(m_activeBackendId, nullptr);
}

QString SpeechService::activeBackendId() const
{
    return m_activeBackendId;
}

// ============================================================================
// 后端选择
//
//  选择逻辑刻意写在两个阶段：
//    1. 先看用户指定的后端能不能用（尊重用户选择）；
//    2. 不能用就按"能力强的优先"自动降级，并把降级原因记录下来。
//
//  "能力强的优先"排序：Full > SynthesisOnly/RecognitionOnly > RuleBasedOnly。
//  这么排是因为 SAPI 能真朗读，而规则后端只能匹配词表；
//  能朗读的功能是用户能立刻感知到价值的。
// ============================================================================
QString SpeechService::selectBackend(const QString &preferredId)
{
    // 阶段 1：用户指定
    if (!preferredId.trimmed().isEmpty()) {
        SpeechBackend *b = m_backends.value(preferredId.trimmed(), nullptr);
        if (b && b->kind() != SpeechBackendKind::None) {
            m_activeBackendId = b->id();
            return m_activeBackendId;
        }
        if (b) {
            DC_INFO(LogCat::Speech, QStringLiteral("用户指定的语音后端 %1 不可用（%2），将自动选择")
                        .arg(preferredId, b->unavailableReason()));
        } else {
            DC_WARN(LogCat::Speech, QStringLiteral("用户指定的语音后端 %1 不存在").arg(preferredId));
        }
    }

    // 阶段 2：按能力排序自动选择
    auto kindScore = [](SpeechBackendKind k) -> int {
        switch (k) {
        case SpeechBackendKind::Full:            return 4;
        case SpeechBackendKind::RecognitionOnly: return 3;
        case SpeechBackendKind::SynthesisOnly:   return 3;
        case SpeechBackendKind::RuleBasedOnly:   return 1;
        case SpeechBackendKind::None:            return 0;
        }
        return 0;
    };

    QString bestId;
    int bestScore = 0;

    for (auto it = m_backends.constBegin(); it != m_backends.constEnd(); ++it) {
        const int score = kindScore(it.value()->kind());
        if (score > bestScore) {
            bestScore = score;
            bestId = it.key();
        }
    }

    m_activeBackendId = bestId;
    return m_activeBackendId;
}

// ============================================================================
// 状态汇总
// ============================================================================
void SpeechService::recomputeStatus()
{
    SpeechBackend *active = activeBackend();

    const SpeechServiceStatus previous = m_status;
    m_status = SpeechServiceStatus{};
    m_status.backendCount = m_backends.size();

    if (!active) {
        m_status.backendReady = false;
        m_status.kind = SpeechBackendKind::None;
        m_status.canRecognize = false;
        m_status.canSynthesize = false;
        m_status.limitation = tr("没有可用的语音后端。");
    } else {
        m_status.kind = active->kind();
        m_status.activeBackendId = active->id();
        m_status.activeBackendName = active->displayName();
        m_status.backendReady = active->kind() != SpeechBackendKind::None;

        // 注意配置开关：用户在设置里关掉了朗读，那 canSynthesize 就该是 false，
        // 哪怕后端本身能朗读。这样界面才会正确置灰按钮。
        m_status.canSynthesize = active->canSynthesize() && m_config.synthesisEnabled;
        m_status.canRecognize = active->canRecognize() && m_config.recognitionEnabled;

        // 拼装"能力受限说明"。能用的部分要说清楚，不能用的部分要说原因。
        QStringList notes;

        if (!m_config.synthesisEnabled) {
            notes << tr("朗读功能已在设置中关闭。");
        } else if (!active->canSynthesize()) {
            const QString reason = active->unavailableReason();
            notes << (reason.isEmpty() ? tr("当前后端不支持朗读。") : reason);
        }

        if (!m_config.recognitionEnabled) {
            notes << tr("语音输入功能已在设置中关闭。");
        } else if (!active->canRecognize()) {
            const QString reason = active->unavailableReason();
            notes << (reason.isEmpty() ? tr("当前后端不支持语音输入。") : reason);
        } else if (active->kind() == SpeechBackendKind::RuleBasedOnly) {
            // 这一条必须显式展示 —— 否则用户会以为这是真正的语音识别
            notes << tr("当前使用的是规则匹配后端：只能识别少量固定的说法"
                        "（如「打开」「搜索」「翻译」），"
                        "不是真正的语音识别。");
        }

        m_status.limitation = notes.join(QLatin1Char('\n'));
    }

    if (previous.limitation != m_status.limitation) {
        emit limitationChanged(m_status.limitation);
    }
}

// ============================================================================
// 能力查询
// ============================================================================
bool SpeechService::canRecognize() const
{
    return m_status.canRecognize;
}

bool SpeechService::canSynthesize() const
{
    return m_status.canSynthesize;
}

QString SpeechService::recognitionLimitation() const
{
    if (m_status.canRecognize) {
        if (m_status.kind == SpeechBackendKind::RuleBasedOnly) {
            return tr("仅支持少量固定指令（规则匹配），不是真正的语音识别。");
        }
        return QString();
    }
    if (!m_config.recognitionEnabled) {
        return tr("语音输入已在设置中关闭。");
    }
    SpeechBackend *active = activeBackend();
    if (!active) {
        return tr("没有可用的语音后端。");
    }
    const QString reason = active->unavailableReason();
    return reason.isEmpty() ? tr("当前后端不支持语音输入。") : reason;
}

QString SpeechService::synthesisLimitation() const
{
    if (m_status.canSynthesize) {
        return QString();
    }
    if (!m_config.synthesisEnabled) {
        return tr("朗读已在设置中关闭。");
    }
    SpeechBackend *active = activeBackend();
    if (!active) {
        return tr("没有可用的语音后端。");
    }
    const QString reason = active->unavailableReason();
    return reason.isEmpty() ? tr("当前后端不支持朗读。") : reason;
}

// ============================================================================
// 朗读
// ============================================================================
QString SpeechService::speak(const QString &text, const QString &language)
{
    const QString requestId = QStringLiteral("spk-%1-%2")
                                  .arg(QDateTime::currentMSecsSinceEpoch())
                                  .arg(++m_counter);

    if (text.trimmed().isEmpty()) {
        // 没内容可读，不算失败
        QTimer::singleShot(0, this, [this, requestId]() {
            emit speechFinished(requestId, true, QString());
        });
        return requestId;
    }

    SpeechBackend *active = activeBackend();
    if (!active || !m_status.canSynthesize) {
        const QString reason = synthesisLimitation();
        QTimer::singleShot(0, this, [this, requestId, reason]() {
            emit speechFinished(requestId, false, reason);
        });
        return requestId;
    }

    // 已经在读的话先停掉旧的 —— 连续朗读时用户想听的是最新那条，
    // 排队念完之前所有内容会很烦人。
    if (active->isSpeaking()) {
        active->stopPlayback();
    }

    m_pendingSpeakId = requestId;

    SynthesisRequest request;
    request.text = text;
    request.language = language.isEmpty() ? m_config.language : language;
    request.voiceId = m_config.voiceId;
    request.rate = m_config.rate;
    request.volume = m_config.volume;

    active->synthesize(request);
    return requestId;
}

void SpeechService::stopSpeaking()
{
    SpeechBackend *active = activeBackend();
    if (active) {
        active->stopPlayback();
    }
}

bool SpeechService::isSpeaking() const
{
    SpeechBackend *active = activeBackend();
    return active ? active->isSpeaking() : false;
}

// ============================================================================
// 识别
// ============================================================================
QString SpeechService::listen(const QByteArray &pcmData, int sampleRate,
                              const QString &language)
{
    const QString requestId = QStringLiteral("rec-%1-%2")
                                  .arg(QDateTime::currentMSecsSinceEpoch())
                                  .arg(++m_counter);

    SpeechBackend *active = activeBackend();
    if (!active || !m_status.canRecognize) {
        const QString reason = recognitionLimitation();
        RecognitionResult result;
        result.ok = false;
        result.errorCode = ec(erc::SpeechNoBackend);
        result.message = reason;
        QTimer::singleShot(0, this, [this, requestId, result]() {
            emit recognitionFinished(requestId, result);
        });
        return requestId;
    }

    if (pcmData.isEmpty()) {
        // 没录到音频。这不是错误，是"用户没说话"。
        RecognitionResult result;
        result.ok = true;
        result.errorCode = ec(erc::Ok);
        result.text.clear();
        result.message = tr("没有录到音频内容。");
        QTimer::singleShot(0, this, [this, requestId, result]() {
            emit recognitionFinished(requestId, result);
        });
        return requestId;
    }

    m_pendingRecognitionId = requestId;

    RecognitionRequest request;
    request.pcmData = pcmData;
    request.sampleRate = sampleRate;
    request.language = language.isEmpty() ? m_config.language : language;

    active->recognize(request);
    return requestId;
}

void SpeechService::cancelListening()
{
    SpeechBackend *active = activeBackend();
    if (active) {
        active->cancelRecognition();
    }
    m_pendingRecognitionId.clear();
}

// ============================================================================
// 是否需要用户确认
//
//  两条独立的触发条件（满足任一即需要确认）：
//    1. 置信度低于配置阈值 —— 识别得不确定，不该擅自执行；
//    2. 来自规则匹配后端 —— 它只是关键词命中，且用户可能没有授权
//       让规则后端直接执行命令（allowRuleBasedCommandExecution 默认 false）。
//
//  这实现的是 SRS 4.10「识别置信度不足时应请用户确认，而不是直接执行命令」。
// ============================================================================
bool SpeechService::needsConfirmation(const RecognitionResult &result) const
{
    // 识别本身失败的情况由调用方处理，不在这里判断
    if (!result.ok) {
        return false;
    }

    if (result.ruleBased && !m_config.allowRuleBasedCommandExecution) {
        return true;
    }

    // confidence < 0 表示后端没提供置信度。这种情况按"需要确认"处理 —— 
    // 不知道有多确定的时候，保守一点比擅自执行好。
    if (result.confidence < 0.0) {
        return true;
    }

    return result.confidence < m_config.confidenceThreshold;
}

// ============================================================================
// 音色
// ============================================================================
QList<VoiceInfo> SpeechService::availableVoices() const
{
    SpeechBackend *active = activeBackend();
    return active ? active->availableVoices() : QList<VoiceInfo>();
}

bool SpeechService::setVoice(const QString &voiceId)
{
    SpeechBackend *active = activeBackend();
    if (!active) {
        return false;
    }
    if (auto *sapi = qobject_cast<SapiSpeechBackend *>(active)) {
        if (sapi->setVoiceById(voiceId)) {
            m_config.voiceId = voiceId;
            return true;
        }
        return false;
    }
    return false;
}

// ============================================================================
// 诊断报告
// ============================================================================
QString SpeechService::diagnosticReport() const
{
    QStringList lines;

    lines << tr("活动后端：%1")
                 .arg(m_status.activeBackendName.isEmpty()
                          ? tr("(无)")
                          : m_status.activeBackendName);
    lines << tr("能力等级：%1").arg(speechBackendKindToString(m_status.kind));
    lines << tr("语音输入：%1")
                 .arg(m_status.canRecognize ? tr("可用") : tr("不可用"));
    lines << tr("语音朗读：%1")
                 .arg(m_status.canSynthesize ? tr("可用") : tr("不可用"));

    if (!m_status.limitation.isEmpty()) {
        lines << QString();
        lines << tr("能力说明：");
        lines << m_status.limitation;
    }

    lines << QString();
    lines << tr("已注册后端（%1 个）：").arg(m_backends.size());
    for (auto it = m_backends.constBegin(); it != m_backends.constEnd(); ++it) {
        const SpeechBackend *b = it.value();
        QString line = QStringLiteral("  %1 — %2")
                           .arg(b->id(), b->displayName());
        if (b->kind() == SpeechBackendKind::None) {
            const QString reason = b->unavailableReason();
            line += reason.isEmpty() ? tr("（不可用）") : tr("（不可用：%1）").arg(reason);
        } else {
            line += QStringLiteral(" — %1").arg(speechBackendKindToString(b->kind()));
        }
        lines << line;
    }

    lines << QString();
    lines << tr("可用音色：%1 个").arg(availableVoices().size());

    return lines.join(QLatin1Char('\n'));
}

} // namespace dc
