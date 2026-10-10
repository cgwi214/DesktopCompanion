#include "SpeechBackend.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QTimer>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// 能力分级与文本/结构体的转换
// ============================================================================
QString speechBackendKindToString(SpeechBackendKind kind)
{
    switch (kind) {
    case SpeechBackendKind::Full:            return QStringLiteral("full");
    case SpeechBackendKind::SynthesisOnly:   return QStringLiteral("synthesis_only");
    case SpeechBackendKind::RecognitionOnly: return QStringLiteral("recognition_only");
    case SpeechBackendKind::RuleBasedOnly:   return QStringLiteral("rule_based_only");
    case SpeechBackendKind::None:            return QStringLiteral("none");
    }
    // 未知值 -> 最保守的 none（失败安全）
    return QStringLiteral("none");
}

SpeechBackendKind speechBackendKindFromString(const QString &text)
{
    const QString t = text.trimmed().toLower();
    if (t == QLatin1String("full"))             return SpeechBackendKind::Full;
    if (t == QLatin1String("synthesis_only"))   return SpeechBackendKind::SynthesisOnly;
    if (t == QLatin1String("recognition_only")) return SpeechBackendKind::RecognitionOnly;
    if (t == QLatin1String("rule_based_only"))  return SpeechBackendKind::RuleBasedOnly;
    return SpeechBackendKind::None;
}

SpeechCapability toModelCapability(SpeechBackendKind kind,
                                   const QString &backendId,
                                   const QString &backendDisplayName,
                                   const QString &unavailableReason)
{
    SpeechCapability cap;
    cap.backendId = backendId;
    cap.backendDisplayName = backendDisplayName;
    cap.unavailableReason = unavailableReason;

    switch (kind) {
    case SpeechBackendKind::Full:
        cap.recognitionAvailable = true;
        cap.synthesisAvailable = true;
        cap.pushToTalkAvailable = true;
        break;
    case SpeechBackendKind::SynthesisOnly:
        cap.recognitionAvailable = false;
        cap.synthesisAvailable = true;
        break;
    case SpeechBackendKind::RecognitionOnly:
        cap.recognitionAvailable = true;
        cap.synthesisAvailable = false;
        cap.pushToTalkAvailable = true;
        break;
    case SpeechBackendKind::RuleBasedOnly:
        // 注意：这里把 recognitionAvailable 设为 true，
        // 但同时保留 unavailableReason 说明它的局限性。
        // 界面必须同时展示原因，否则用户会以为这是完整识别能力。
        cap.recognitionAvailable = true;
        cap.synthesisAvailable = false;
        break;
    case SpeechBackendKind::None:
        break;
    }
    return cap;
}

// ============================================================================
// RuleBasedSpeechBackend
// ============================================================================
RuleBasedSpeechBackend::RuleBasedSpeechBackend(QObject *parent)
    : SpeechBackend(parent)
{
    loadBuiltinPhrases();
}

RuleBasedSpeechBackend::~RuleBasedSpeechBackend() = default;

QString RuleBasedSpeechBackend::id() const
{
    return QStringLiteral("rule_based");
}

QString RuleBasedSpeechBackend::displayName() const
{
    return tr("规则匹配（有限指令）");
}

SpeechBackendKind RuleBasedSpeechBackend::kind() const
{
    return SpeechBackendKind::RuleBasedOnly;
}

void RuleBasedSpeechBackend::addPhrase(const QString &spokenForm, const QString &canonical)
{
    const QString key = spokenForm.trimmed().toLower();
    if (key.isEmpty() || canonical.trimmed().isEmpty()) {
        return;
    }
    m_phrases.insert(key, canonical.trimmed());
}

int RuleBasedSpeechBackend::phraseCount() const
{
    return m_phrases.size();
}

// ---------------------------------------------------------------------------
// 内置短语表
//
//  这些是"用户最可能用嘴说出来的说法"到"规范指令词"的映射。
//  注意左边是口语化的说法（"把记事本打开"），右边是命令引擎认识的词
//  （"打开"）。这样即使没有真正的语音识别，也能覆盖一小部分高频场景。
// ---------------------------------------------------------------------------
void RuleBasedSpeechBackend::loadBuiltinPhrases()
{
    struct Pair { const char *spoken; const char *canonical; };
    static const Pair kPhrases[] = {
        // 中文 —— 打开类
        { "打开",         "打开" },
        { "开一下",       "打开" },
        { "帮我打开",     "打开" },
        { "把东西打开",   "打开" },
        { "启动",         "打开" },
        { "运行",         "打开" },
        // 中文 —— 搜索类
        { "搜索",         "搜索" },
        { "搜一下",       "搜索" },
        { "查一下",       "搜索" },
        { "帮我查",       "搜索" },
        { "找一下",       "搜索" },
        // 中文 —— 翻译类
        { "翻译",         "翻译" },
        { "翻一下",       "翻译" },
        { "帮我翻译",     "翻译" },
        // 中文 —— 录音类
        { "录音",         "录音" },
        { "开始录音",     "录音" },
        { "停止录音",     "停止录音" },
        // 中文 —— 历史与设置
        { "历史",         "历史" },
        { "历史记录",     "历史" },
        { "设置",         "设置" },
        { "打开设置",     "设置" },
        // 中文 —— 桌宠状态
        { "隐藏",         "隐藏" },
        { "躲起来",       "隐藏" },
        { "休息",         "隐藏" },
        { "出来",         "显示" },
        // 英文
        { "open",         "打开" },
        { "launch",       "打开" },
        { "start",        "打开" },
        { "run",          "打开" },
        { "search",       "搜索" },
        { "look up",      "搜索" },
        { "google",       "搜索" },
        { "translate",    "翻译" },
        { "record",       "录音" },
        { "stop",         "停止录音" },
        { "history",      "历史" },
        { "settings",     "设置" },
        { "hide",         "隐藏" },
        { "show",         "显示" },
    };

    for (const Pair &p : kPhrases) {
        addPhrase(QString::fromUtf8(p.spoken), QString::fromUtf8(p.canonical));
    }

    DC_INFO(LogCat::Speech, QStringLiteral("规则匹配后端已加载 %1 条短语").arg(m_phrases.size()));
}

// ---------------------------------------------------------------------------
// 识别
//
//  说明：本后端不做音频转文本。它接受的是"已经是文本的输入"，
//  用来做分词与短语归并。真正的音频转文本由 RecorderService 处理
//  （用系统能力或外部工具），或者根本不做 —— 那是设计选择，不是缺陷。
//
//  但为了不误导调用方，如果传进来的是原始 PCM 数据（不是一个可读文本），
//  这里会明确返回失败并说明原因，而不是假装成功。
// ---------------------------------------------------------------------------
void RuleBasedSpeechBackend::recognize(const RecognitionRequest &request)
{
    QTimer::singleShot(0, this, [this, request]() {
        const qint64 started = QDateTime::currentMSecsSinceEpoch();

        RecognitionResult result;

        // 判断输入是文本还是原始音频。
        // 简单判据：看有没有 NUL 或不可打印字符占比是否很高。
        int nonPrintable = 0;
        const int probeLen = qMin(request.pcmData.size(), 256);
        for (int i = 0; i < probeLen; ++i) {
            const unsigned char c = static_cast<unsigned char>(request.pcmData.at(i));
            if (c < 0x09 || (c > 0x0D && c < 0x20)) {
                ++nonPrintable;
            }
        }

        const bool looksLikeRawAudio = probeLen > 0
                                       && (nonPrintable > probeLen / 10);

        if (looksLikeRawAudio) {
            result.ok = false;
            result.errorCode = ec(erc::SpeechUnavailable);
            result.message = tr(
                "规则匹配后端无法把音频转成文字。"
                "它只能匹配已经识别出的文本。"
                "如需真正的语音识别，请在本机安装并配置一个语音识别后端。");
            result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
            emit recognitionFinished(result);
            return;
        }

        const QString input = QString::fromUtf8(request.pcmData).trimmed();
        if (input.isEmpty()) {
            // "听到了但没内容" —— ok=true 但 text 为空
            result.ok = true;
            result.errorCode = ec(erc::Ok);
            result.text.clear();
            result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
            emit recognitionFinished(result);
            return;
        }

        // 在输入里找已注册的短语。
        // 用"包含"而不是"等于"，因为口语通常是"帮我打开记事本"这种形式，
        // 短语是嵌在句子里的。
        const QString lower = input.toLower();
        QString bestMatch;
        int longest = 0;
        for (auto it = m_phrases.constBegin(); it != m_phrases.constEnd(); ++it) {
            if (it.key().isEmpty()) {
                continue;
            }
            if (lower.contains(it.key()) && it.key().length() > longest) {
                longest = it.key().length();
                bestMatch = it.value();
            }
        }

        if (bestMatch.isEmpty()) {
            result.ok = false;
            result.errorCode = ec(erc::SpeechLowConfidence);
            result.message = tr(
                "没有匹配到可识别的指令。规则匹配后端只认识少量固定说法"
                "（例如「打开」「搜索」「翻译」「录音」），"
                "更自然的说法需要真正的语音识别。");
            result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;
            emit recognitionFinished(result);
            return;
        }

        result.ok = true;
        result.errorCode = ec(erc::Ok);
        result.text = bestMatch;
        // 置信度给 0.5：明确表示"这只是关键词命中，不确定"。
        // 上层据此决定是否需要用户确认，尤其对中等风险以上操作。
        result.confidence = 0.5;
        result.ruleBased = true;
        result.elapsedMs = QDateTime::currentMSecsSinceEpoch() - started;

        DC_INFO(LogCat::Speech, QStringLiteral("规则匹配命中短语，置信度 0.5"));
        emit recognitionFinished(result);
    });
}

// ---------------------------------------------------------------------------
// 合成：本后端不做语音合成，如实告知
// ---------------------------------------------------------------------------
void RuleBasedSpeechBackend::synthesize(const SynthesisRequest &request)
{
    Q_UNUSED(request)
    QTimer::singleShot(0, this, [this]() {
        SynthesisResult result;
        result.ok = false;
        result.errorCode = ec(erc::SpeechUnavailable);
        result.message = tr("规则匹配后端不提供语音朗读能力。");
        emit synthesisFinished(result);
    });
}

} // namespace dc
