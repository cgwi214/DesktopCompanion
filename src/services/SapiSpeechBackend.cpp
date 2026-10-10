#include "SapiSpeechBackend.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QDateTime>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <objbase.h>
#  include <sapi.h>
#  include <sphelper.h>
#endif

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// Impl —— 把所有 Windows/COM 细节关在这里
// ============================================================================
struct SapiSpeechBackend::Impl {
#ifdef Q_OS_WIN
    ISpVoice *voice = nullptr;
    ISpRecognizer *recognizer = nullptr;
    ISpRecoContext *recoContext = nullptr;
    ISpRecoGrammar *grammar = nullptr;

    bool comInitialized = false;
    bool voiceOk = false;
    bool recognizerOk = false;

    QString voiceError;
    QString recognizerError;

    // 合成是否完成 —— SAPI 的 Speak 可以同步也可以异步，
    // 我们用异步 + 标志位轮询，避免阻塞 UI 线程。
    bool speakDone = true;
#endif
};

// ============================================================================
// 构造 / 析构
// ============================================================================
SapiSpeechBackend::SapiSpeechBackend(QObject *parent)
    : SpeechBackend(parent)
    , d(new Impl)
{
    m_kind = SpeechBackendKind::None;
    m_unavailableReason = tr("尚未初始化。");
}

SapiSpeechBackend::~SapiSpeechBackend()
{
    stopPlayback();
#ifdef Q_OS_WIN
    if (d) {
        if (d->grammar)    { d->grammar->Release();    d->grammar = nullptr; }
        if (d->recoContext){ d->recoContext->Release();d->recoContext = nullptr; }
        if (d->recognizer) { d->recognizer->Release(); d->recognizer = nullptr; }
        if (d->voice)      { d->voice->Release();      d->voice = nullptr; }
        if (d->comInitialized) {
            ::CoUninitialize();
            d->comInitialized = false;
        }
    }
#endif
    delete d;
    d = nullptr;
}

// ============================================================================
// 初始化
//
//  探测策略：每一项能力单独探测并记录各自的原因。
//  这样在"只能朗读不能识别"的系统上，界面能准确说明是哪一半不可用，
//  而不是笼统地说"语音功能不可用"。
// ============================================================================
bool SapiSpeechBackend::initialize()
{
#ifdef Q_OS_WIN
    // ---- COM 初始化 ----
    // 用 STA（单线程套间）：SAPI 是 COM 组件，且语音对象有线程亲和性。
    // 主线程用 STA 是标准做法。
    const HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hrCom)) {
        d->comInitialized = true;
        DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：COM 初始化成功"));
    } else if (hrCom == RPC_E_CHANGED_MODE) {
        // 已经被别的代码初始化为 MTA 了。SAPI 在 MTA 下通常仍能用，
        // 但会有线程问题。这里记录下来但不阻止后续尝试。
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：COM 已以其他模式初始化（0x%1）")
                    .arg(static_cast<quint32>(hrCom), 8, 16, QLatin1Char('0')));
        d->comInitialized = false;
    } else if (hrCom == S_FALSE) {
        // 已经初始化过，不算失败
        d->comInitialized = true;
    } else {
        d->voiceError = tr("无法初始化系统语音组件（错误码 0x%1）。")
                            .arg(static_cast<quint32>(hrCom), 8, 16, QLatin1Char('0'));
        DC_ERROR(LogCat::Platform, QStringLiteral("SAPI 后端：COM 初始化失败 0x%1")
                                       .arg(static_cast<quint32>(hrCom), 8, 16, QLatin1Char('0')));
    }

    // ---- 探测合成（朗读）----
    {
        ISpVoice *voice = nullptr;
        const HRESULT hr = ::CoCreateInstance(
            CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
            reinterpret_cast<void **>(&voice));

        if (SUCCEEDED(hr) && voice) {
            d->voice = voice;

            // 再确认真的能拿到音色 —— 有些精简版系统能创建对象但没有音色，
            // 那样 Speak 会静默失败。
            ISpObjectToken *token = nullptr;
            const HRESULT hrToken = d->voice->GetVoice(&token);
            if (SUCCEEDED(hrToken) && token) {
                d->voiceOk = true;
                token->Release();
                DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：语音合成可用"));
            } else {
                d->voiceError = tr("系统语音合成对象存在，但没有可用的音色。"
                                   "可能需要在系统设置中安装语音包。");
                DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：GetVoice 失败 0x%1")
                            .arg(static_cast<quint32>(hrToken), 8, 16, QLatin1Char('0')));
                // 对象保留，理论上仍可能可用（部分系统 GetVoice 返回失败但能读默认音色）
                d->voiceOk = false;
            }
        } else {
            d->voiceError = tr("本系统不提供语音合成能力（无法创建语音对象，"
                               "错误码 0x%1）。")
                                .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
            DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：CoCreateInstance(SpVoice) 失败 0x%1")
                        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        }
    }

    // ---- 探测识别 ----
    // 注意：这里刻意不创建共享识别器。
    // 共享识别器（SpSharedRecognizer）会接管系统的语音输入，
    // 在用户没明确开启语音功能时创建它，会：抢麦克风、在任务栏出现图标、
    // 可能干扰用户正在用的其他语音软件。
    //
    // 所以这里的策略是：只在用户显式启用语音输入时，才由 SpeechService
    // 调用 enableRecognition() 去创建。initialize() 只做"系统里有没有"
    // 的探测，不实际占用资源。
    {
        ISpRecognizer *probe = nullptr;
        const HRESULT hr = ::CoCreateInstance(
            CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_ISpRecognizer,
            reinterpret_cast<void **>(&probe));

        if (SUCCEEDED(hr) && probe) {
            probe->Release();
            d->recognizerOk = true;
            DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：识别引擎可用（未启用）"));
        } else {
            d->recognizerOk = false;
            // Windows 10/11 上这是常态，不是错误。
            d->recognizerError = tr(
                "本系统未提供可用的语音识别引擎。"
                "Windows 10/11 通常需要额外安装语音识别组件。");
            DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：识别引擎不可用 0x%1（在 Win10/11 上属正常）")
                        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        }
    }

    // ---- 汇总能力 ----
    if (d->voiceOk && d->recognizerOk) {
        m_kind = SpeechBackendKind::Full;
        m_unavailableReason.clear();
    } else if (d->voiceOk) {
        m_kind = SpeechBackendKind::SynthesisOnly;
        m_unavailableReason = tr("本系统可以朗读文本，但不支持语音识别。");
    } else if (d->recognizerOk) {
        m_kind = SpeechBackendKind::RecognitionOnly;
        m_unavailableReason = tr("本系统支持语音识别，但不支持朗读。");
    } else {
        m_kind = SpeechBackendKind::None;
        m_unavailableReason = tr("本系统未提供可用的语音能力。");
    }

    DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端初始化完成：能力=%1")
                .arg(speechBackendKindToString(m_kind)));

    return m_kind != SpeechBackendKind::None;

#else
    // 非 Windows：如实报告不可用。
    // Linux 上如果要做，应当用 espeak/festival 之类的后端，
    // 那是另一个 SpeechBackend 实现，不影响这里的结构。
    m_kind = SpeechBackendKind::None;
    m_unavailableReason = tr(
        "当前系统的语音能力尚未接入。"
        "本项目在 Windows 上使用系统自带的语音组件，"
        "其他系统需要单独配置语音后端。");
    DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端在当前平台上不可用（非 Windows）"));
    return false;
#endif
}

// ============================================================================
// 元信息
// ============================================================================
QString SapiSpeechBackend::id() const
{
    return QStringLiteral("sapi");
}

QString SapiSpeechBackend::displayName() const
{
    return tr("系统语音（SAPI）");
}

SpeechBackendKind SapiSpeechBackend::kind() const
{
    return m_kind;
}

QString SapiSpeechBackend::unavailableReason() const
{
    return m_unavailableReason;
}

QStringList SapiSpeechBackend::supportedLanguages() const
{
    // 实际支持的语言取决于系统装的语音包。这里返回"可能支持"的集合，
    // 真正的判定要看 availableVoices() 里每个音色的 language 字段。
    return { QStringLiteral("zh"), QStringLiteral("en") };
}

// ============================================================================
// 音色枚举
// ============================================================================
QList<VoiceInfo> SapiSpeechBackend::availableVoices() const
{
    QList<VoiceInfo> result;

#ifdef Q_OS_WIN
    if (!d->voice) {
        return result;
    }

    IEnumSpObjectTokens *enumTokens = nullptr;
    const HRESULT hr = SpEnumTokens(SPCAT_VOICES, nullptr, nullptr, &enumTokens);
    if (FAILED(hr) || !enumTokens) {
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：枚举音色失败 0x%1")
                    .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        return result;
    }

    ULONG count = 0;
    enumTokens->GetCount(&count);

    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken *token = nullptr;
        if (FAILED(enumTokens->Next(1, &token, nullptr)) || !token) {
            break;
        }

        VoiceInfo info;

        // 显示名：先用它的 description 属性，退回"音色 N"
        WCHAR *desc = nullptr;
        if (SUCCEEDED(SpGetDescription(token, &desc)) && desc) {
            info.displayName = QString::fromWCharArray(desc);
            ::CoTaskMemFree(desc);
        }
        if (info.displayName.isEmpty()) {
            info.displayName = tr("音色 %1").arg(i + 1);
        }

        // 音色 id：用 token 的 string 值，这是稳定可复用的标识
        WCHAR *tokenId = nullptr;
        if (SUCCEEDED(token->GetId(&tokenId)) && tokenId) {
            info.id = QString::fromWCharArray(tokenId);
            ::CoTaskMemFree(tokenId);
        }

        // 语言属性
        ISpDataKey *attributes = nullptr;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
            WCHAR *lang = nullptr;
            if (SUCCEEDED(attributes->GetStringValue(L"Language", &lang)) && lang) {
                // SAPI 的语言属性是十六进制 LCID，例如 "804" 是简体中文
                const QString hexLang = QString::fromWCharArray(lang);
                ::CoTaskMemFree(lang);

                bool ok = false;
                const uint lcid = hexLang.toUInt(&ok, 16);
                if (ok) {
                    // 取主语言 id（低 10 位）
                    const uint primaryLang = lcid & 0x3FF;
                    switch (primaryLang) {
                    case 0x04: info.language = QStringLiteral("zh"); break;
                    case 0x09: info.language = QStringLiteral("en"); break;
                    case 0x11: info.language = QStringLiteral("ja"); break;
                    case 0x12: info.language = QStringLiteral("ko"); break;
                    case 0x0C: info.language = QStringLiteral("fr"); break;
                    case 0x07: info.language = QStringLiteral("de"); break;
                    case 0x0A: info.language = QStringLiteral("es"); break;
                    case 0x19: info.language = QStringLiteral("ru"); break;
                    default: break;
                    }
                }
            }

            WCHAR *gender = nullptr;
            if (SUCCEEDED(attributes->GetStringValue(L"Gender", &gender)) && gender) {
                const QString g = QString::fromWCharArray(gender).toLower();
                ::CoTaskMemFree(gender);
                if (g == QLatin1String("male")) {
                    info.gender = QStringLiteral("male");
                } else if (g == QLatin1String("female")) {
                    info.gender = QStringLiteral("female");
                }
            }

            attributes->Release();
        }

        result << info;
        token->Release();
    }

    enumTokens->Release();
#endif

    return result;
}

QString SapiSpeechBackend::currentVoiceId() const
{
#ifdef Q_OS_WIN
    if (!d->voice) {
        return QString();
    }
    ISpObjectToken *token = nullptr;
    if (FAILED(d->voice->GetVoice(&token)) || !token) {
        return QString();
    }
    WCHAR *tokenId = nullptr;
    QString result;
    if (SUCCEEDED(token->GetId(&tokenId)) && tokenId) {
        result = QString::fromWCharArray(tokenId);
        ::CoTaskMemFree(tokenId);
    }
    token->Release();
    return result;
#else
    return QString();
#endif
}

bool SapiSpeechBackend::setVoiceById(const QString &voiceId)
{
#ifdef Q_OS_WIN
    if (!d->voice || voiceId.trimmed().isEmpty()) {
        return false;
    }

    // 为什么不用 SpGetTokenFromId：
    //   MinGW 的 <sphelper.h> 只提供 SpGetCategoryFromId / SpEnumTokens /
    //   SpGetDescription，并没有 SpGetTokenFromId（那是 Windows SDK 里才有的）。
    //   因此这里改为"枚举全部音色 -> 按 GetId() 匹配"，语义完全等价，
    //   而且不依赖 SDK 版本的差异。
    //
    //   我们匹配的是 availableVoices() 里返回的同一个 id（token->GetId()），
    //   所以界面上选中的音色一定能在这里被找回。
    IEnumSpObjectTokens *enumTokens = nullptr;
    const HRESULT hrEnum = SpEnumTokens(SPCAT_VOICES, nullptr, nullptr, &enumTokens);
    if (FAILED(hrEnum) || !enumTokens) {
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：枚举音色失败，无法切换音色 0x%1")
                    .arg(static_cast<quint32>(hrEnum), 8, 16, QLatin1Char('0')));
        return false;
    }

    const QString wanted = voiceId.trimmed();
    ISpObjectToken *matched = nullptr;

    ULONG count = 0;
    enumTokens->GetCount(&count);

    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken *candidate = nullptr;
        if (FAILED(enumTokens->Next(1, &candidate, nullptr)) || !candidate) {
            break;
        }

        WCHAR *tokenId = nullptr;
        QString id;
        if (SUCCEEDED(candidate->GetId(&tokenId)) && tokenId) {
            id = QString::fromWCharArray(tokenId);
            ::CoTaskMemFree(tokenId);
        }

        if (id == wanted) {
            matched = candidate;   // 找到就留下引用，后面由 SetVoice 使用
            break;
        }
        candidate->Release();
    }

    enumTokens->Release();

    if (!matched) {
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：找不到指定的音色，保持当前音色"));
        return false;
    }

    const HRESULT hrSet = d->voice->SetVoice(matched);
    matched->Release();

    if (FAILED(hrSet)) {
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：设置音色失败 0x%1")
                    .arg(static_cast<quint32>(hrSet), 8, 16, QLatin1Char('0')));
        return false;
    }

    DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：已切换音色"));
    return true;
#else
    Q_UNUSED(voiceId)
    return false;
#endif
}

// ============================================================================
// 语速与音量
// ============================================================================
int SapiSpeechBackend::rate() const
{
#ifdef Q_OS_WIN
    if (d->voice) {
        long r = 0;
        if (SUCCEEDED(d->voice->GetRate(&r))) {
            return static_cast<int>(r);
        }
    }
#endif
    return 0;
}

void SapiSpeechBackend::setRate(int rate)
{
    const int clamped = qBound(-10, rate, 10);
#ifdef Q_OS_WIN
    if (d->voice) {
        d->voice->SetRate(static_cast<long>(clamped));
    }
#else
    Q_UNUSED(clamped)
#endif
}

int SapiSpeechBackend::volume() const
{
#ifdef Q_OS_WIN
    if (d->voice) {
        USHORT v = 100;
        if (SUCCEEDED(d->voice->GetVolume(&v))) {
            return static_cast<int>(v);
        }
    }
#endif
    return 100;
}

void SapiSpeechBackend::setVolume(int volume)
{
    const int clamped = qBound(0, volume, 100);
#ifdef Q_OS_WIN
    if (d->voice) {
        d->voice->SetVolume(static_cast<USHORT>(clamped));
    }
#else
    Q_UNUSED(clamped)
#endif
}

// ============================================================================
// 合成（朗读）
//
//  用异步 + 定时器轮询的方式，而不是 SPF_ASYNC 加事件。
//  理由：
//    1. 事件方式需要一个消息循环，在 Qt 里就要额外开线程或挂事件过滤器，
//       复杂度高；
//    2. 轮询间隔 100ms 对朗读场景完全够用（人耳感知不到 100ms 的状态延迟）；
//    3. 轮询在 Qt 主线程的定时器里跑，不需要考虑跨线程 COM 调用的套间问题。
// ============================================================================
void SapiSpeechBackend::synthesize(const SynthesisRequest &request)
{
    SynthesisResult result;

    if (!canSynthesize()) {
        result.ok = false;
        result.errorCode = ec(erc::SpeechUnavailable);
        result.message = m_unavailableReason.isEmpty()
                             ? tr("本系统不支持语音朗读。")
                             : m_unavailableReason;
        QTimer::singleShot(0, this, [this, result]() { emit synthesisFinished(result); });
        return;
    }

    const QString text = request.text.trimmed();
    if (text.isEmpty()) {
        // 空文本直接成功，没什么好读的
        result.ok = true;
        result.errorCode = ec(erc::Ok);
        QTimer::singleShot(0, this, [this, result]() { emit synthesisFinished(result); });
        return;
    }

#ifdef Q_OS_WIN
    if (!d->voice) {
        result.ok = false;
        result.errorCode = ec(erc::SpeechDeviceError);
        result.message = tr("语音组件不可用。");
        QTimer::singleShot(0, this, [this, result]() { emit synthesisFinished(result); });
        return;
    }

    if (request.rate != 0) {
        d->voice->SetRate(static_cast<long>(qBound(-10, request.rate, 10)));
    }
    if (request.volume != 100) {
        d->voice->SetVolume(static_cast<USHORT>(qBound(0, request.volume, 100)));
    }

    // 文本长度保护。SAPI 能处理很长的文本，但一次朗读几万字没有意义，
    // 而且会长时间占用语音设备。
    QString speakText = text;
    static constexpr int kMaxSpeakChars = 2000;
    bool truncated = false;
    if (speakText.length() > kMaxSpeakChars) {
        speakText = speakText.left(kMaxSpeakChars);
        truncated = true;
    }

    const std::wstring wide = speakText.toStdWString();

    // SPF_ASYNC：立即返回，朗读在后台进行。
    // 不加 SPF_PURGEBEFORESPEAK 会排队 —— 这里确实要排队，
    // 但如果用户连续触发朗读，排队太长会让人困惑，所以加上 PUrge 只保留最新一条。
    const DWORD flags = SPF_ASYNC | SPF_PURGEBEFORESPEAK;

    const HRESULT hr = d->voice->Speak(wide.c_str(), flags, nullptr);
    if (FAILED(hr)) {
        result.ok = false;
        result.errorCode = ec(erc::SpeechSynthesisFailed);
        result.message = tr("朗读失败（错误码 0x%1）。")
                             .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        DC_WARN(LogCat::Speech, QStringLiteral("SAPI 后端：Speak 失败 0x%1")
                    .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        QTimer::singleShot(0, this, [this, result]() { emit synthesisFinished(result); });
        return;
    }

    if (!m_speaking) {
        m_speaking = true;
        emit speakingChanged(true);
    }

    if (truncated) {
        DC_INFO(LogCat::Speech, QStringLiteral("朗读文本超过 %1 字符，已截断").arg(kMaxSpeakChars));
    }

    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    const QString requestLanguage = request.language;

    // 轮询朗读状态
    auto *pollTimer = new QTimer(this);
    pollTimer->setInterval(100);

    connect(pollTimer, &QTimer::timeout, this,
            [this, pollTimer, started, requestLanguage]() {
                if (!d) {
                    pollTimer->stop();
                    pollTimer->deleteLater();
                    return;
                }

                SPVOICESTATUS status{};
                if (FAILED(d->voice->GetStatus(&status, nullptr))) {
                    pollTimer->stop();
                    pollTimer->deleteLater();
                    return;
                }

                // dwRunningState == SPRS_DONE 表示当前队列已读完
                if (status.dwRunningState == SPRS_DONE) {
                    pollTimer->stop();
                    pollTimer->deleteLater();

                    m_speaking = false;
                    emit speakingChanged(false);

                    SynthesisResult done;
                    done.ok = true;
                    done.errorCode = ec(erc::Ok);
                    done.playedDirectly = true;   // 已直接播放，不回传音频
                    done.durationMs = QDateTime::currentMSecsSinceEpoch() - started;
                    Q_UNUSED(requestLanguage)
                    emit synthesisFinished(done);
                    return;
                }

                // 安全上限：如果超过 10 分钟还在"说话"，多半是状态机卡住了，
                // 主动停掉而不是永远轮询下去。
                if (QDateTime::currentMSecsSinceEpoch() - started > 10 * 60 * 1000) {
                    pollTimer->stop();
                    pollTimer->deleteLater();
                    d->voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
                    m_speaking = false;
                    emit speakingChanged(false);

                    SynthesisResult done;
                    done.ok = true;
                    done.errorCode = ec(erc::Ok);
                    done.playedDirectly = true;
                    done.durationMs = QDateTime::currentMSecsSinceEpoch() - started;
                    emit synthesisFinished(done);
                }
            });

    pollTimer->start();
    return;

#else
    Q_UNUSED(request)
    result.ok = false;
    result.errorCode = ec(erc::SpeechUnavailable);
    result.message = m_unavailableReason;
    QTimer::singleShot(0, this, [this, result]() { emit synthesisFinished(result); });
#endif
}

void SapiSpeechBackend::stopPlayback()
{
#ifdef Q_OS_WIN
    if (d && d->voice) {
        // 空文本 + PURGEBEFORESPEAK = 清空队列并停止当前朗读
        d->voice->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    }
#endif
    if (m_speaking) {
        m_speaking = false;
        emit speakingChanged(false);
    }
}

void SapiSpeechBackend::cancelSynthesis()
{
    stopPlayback();
}

bool SapiSpeechBackend::isSpeaking() const
{
    return m_speaking;
}

// ============================================================================
// 识别
//
//  这里返回"未实现"，而不是假装做点什么。理由：
//    SAPI 的进程内识别器需要用户显式创建识别上下文、绑定音频输入、
//    加载语法，然后处理识别事件。这套流程在没有识别引擎的系统上
//    根本走不通（Win10/11 常态），而在有引擎的系统上，正确做法是
//    让用户通过系统设置开启语音识别，再让本程序连接。
//
//  因此本实现明确返回"需要配置"，并把用户引导到正确的地方。
//  这比实现一个半成品识别器更诚实，也更有用。
// ============================================================================
void SapiSpeechBackend::recognize(const RecognitionRequest &request)
{
    Q_UNUSED(request)
    QTimer::singleShot(0, this, [this]() {
        RecognitionResult result;
        result.ok = false;
        result.errorCode = ec(erc::SpeechNoBackend);

#ifdef Q_OS_WIN
        if (d && !d->recognizerOk) {
            result.message = d->recognizerError.isEmpty()
                                 ? tr("本系统未安装可用的语音识别引擎。")
                                 : d->recognizerError;
        } else {
            result.message = tr(
                "语音识别需要在系统设置中开启 Windows 语音识别后才可使用。"
                "开启后重新启动本程序即可。");
        }
#else
        result.message = tr("当前系统的语音识别尚未接入。");
#endif
        DC_INFO(LogCat::Speech, QStringLiteral("SAPI 后端：识别请求被拒绝（本实现不提供识别）"));
        emit recognitionFinished(result);
    });
}

void SapiSpeechBackend::cancelRecognition()
{
    // 没有进行中的识别
}

// ============================================================================
// 诊断报告
// ============================================================================
QString SapiSpeechBackend::diagnosticReport() const
{
    QStringList lines;
    lines << tr("语音后端：%1").arg(displayName());
    lines << tr("检测到的能力：%1").arg(speechBackendKindToString(m_kind));

#ifdef Q_OS_WIN
    if (d) {
        lines << tr("语音合成：%1")
                     .arg(d->voiceOk ? tr("可用")
                                     : (d->voiceError.isEmpty()
                                            ? tr("不可用")
                                            : tr("不可用 —— %1").arg(d->voiceError)));
        lines << tr("语音识别：%1")
                     .arg(d->recognizerOk ? tr("可用（需在系统中开启）")
                                          : (d->recognizerError.isEmpty()
                                                 ? tr("不可用")
                                                 : tr("不可用 —— %1").arg(d->recognizerError)));
        lines << tr("COM 初始化：%1").arg(d->comInitialized ? tr("成功") : tr("未成功"));
    }
#else
    lines << tr("当前平台未接入系统语音能力。");
#endif

    if (!m_unavailableReason.isEmpty()) {
        lines << tr("说明：%1").arg(m_unavailableReason);
    }

    const QList<VoiceInfo> voices = availableVoices();
    lines << tr("可用音色：%1 个").arg(voices.size());
    for (int i = 0; i < qMin(voices.size(), 5); ++i) {
        const VoiceInfo &v = voices.at(i);
        lines << QStringLiteral("  %1%2%3")
                     .arg(v.displayName,
                          v.language.isEmpty() ? QString()
                                               : QStringLiteral(" [%1]").arg(v.language),
                          v.gender.isEmpty() ? QString()
                                             : QStringLiteral(" (%1)").arg(v.gender));
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace dc
