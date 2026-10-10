#pragma once

// ============================================================================
//  RecorderService —— 录音（SRS 4.11）
//
//  需求原文（SRS 4.11 录音）：
//    - 录音默认关闭，需用户显式开启；
//    - 录音应有明确的进行中指示；
//    - 录音文件保存在受控目录，并纳入历史记录与清理策略；
//    - 录音时长与占用空间应有上限；
//    - 系统休眠/关机时应安全停止录音。
//
//  本实现的边界（需要如实说明）：
//    本项目不引入任何音频编解码库（会显著增加体积与依赖）。
//    因此录音输出是「无压缩的 WAV（PCM）」。这意味着：
//      - 优点：零依赖、任何播放器都能播、无编码延迟；
//      - 代价：文件很大。16kHz 单声道 16bit 约 32KB/秒，
//              即 1 分钟约 1.9MB，10 分钟约 19MB。
//    这个代价用「时长上限 + 空间上限」来控制：默认最长 10 分钟、
//    单文件上限 50MB，超过就自动停止并明确告知用户原因。
//
//    这与 SRS 对"轻量、无需额外依赖"的要求是一致的取舍。
//    如果将来需要压缩，只需替换 writeAudioData() 这一层。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QDateTime>

#include "model/Types.h"

QT_BEGIN_NAMESPACE
class QFile;
class QTimer;
class QAudioSource;
class QMediaCaptureSession;
class QAudioInput;
class QIODevice;
QT_END_NAMESPACE

namespace dc {

// ---------------------------------------------------------------------------
// 录音配置
// ---------------------------------------------------------------------------
struct RecorderConfig {
    // 总开关。默认 false —— SRS 明确要求录音默认关闭。
    bool enabled = false;

    // 输出目录（必须落在受控目录内，由 CoreRuntime 用 AppPaths 填）
    QString outputDirectory;

    // 音频参数
    int sampleRate = 16000;      // 16kHz 足够语音使用，文件也更小
    int channels = 1;            // 单声道
    int bitsPerSample = 16;

    // 时长上限（秒）。默认 10 分钟。
    int maxDurationSeconds = 600;

    // 单文件大小上限（字节）。默认 50MB。
    qint64 maxFileBytes = 50LL * 1024 * 1024;

    // 录音目录总大小上限（字节）。超过时提示用户清理，但不自动删。
    qint64 maxTotalBytes = 500LL * 1024 * 1024;

    // 是否在录音开始时播放提示音（让用户知道录上了）
    bool playStartBeep = true;

    // 录音文件名前缀
    QString filePrefix = QStringLiteral("recording");
};

// ---------------------------------------------------------------------------
// 录音状态
// ---------------------------------------------------------------------------
enum class RecorderState {
    Idle = 0,        // 空闲
    Starting,        // 正在启动设备
    Recording,       // 录音中
    Stopping,        // 正在收尾写文件
    Unavailable      // 设备或能力不可用
};

QString recorderStateToString(RecorderState state);

// ---------------------------------------------------------------------------
// 录音会话信息
// ---------------------------------------------------------------------------
struct RecordingSession {
    QString id;
    QString filePath;
    QDateTime startedAt;
    QDateTime stoppedAt;
    qint64 bytesWritten = 0;
    int durationMs = 0;
    bool autoStopped = false;      // 是否因超限自动停止
    QString stopReason;            // 自动停止时的原因说明
};

// ---------------------------------------------------------------------------
// 操作结果
// ---------------------------------------------------------------------------
struct RecorderResult {
    bool ok = false;
    QString errorCode;
    QString message;
    RecordingSession session;

    static RecorderResult success(const RecordingSession &session = RecordingSession{})
    {
        RecorderResult r;
        r.ok = true;
        r.errorCode = QString::fromLatin1("DC-0000");
        r.session = session;
        return r;
    }
    static RecorderResult failure(const QString &code, const QString &message)
    {
        RecorderResult r;
        r.ok = false;
        r.errorCode = code;
        r.message = message;
        return r;
    }
};

// ============================================================================
// RecorderService
// ============================================================================
class RecorderService : public QObject
{
    Q_OBJECT

public:
    explicit RecorderService(QObject *parent = nullptr);
    ~RecorderService() override;

    // 应用配置。会校验输出目录是否合法（必须在受控目录内）。
    void applyConfig(const RecorderConfig &config);
    RecorderConfig config() const;

    // 探测录音设备能力。返回是否可用，reason 说明原因。
    bool probeCapability(QString *reason = nullptr) const;

    // ---- 开关 ----
    // 用户显式开启/关闭录音总开关。
    // 关闭时会立即停止正在进行的录音（安全优先）。
    void setEnabled(bool enabled);
    bool isEnabled() const;

    // ---- 录音控制 ----

    // 开始录音。返回结果含 session（id 与文件路径）。
    RecorderResult start();

    // 停止录音并完成文件写入。
    RecorderResult stop(const QString &stopReason = QString());

    // 当前状态
    RecorderState state() const;
    bool isRecording() const;

    // 当前会话信息（未录音时返回空 session）
    RecordingSession currentSession() const;

    // 已录制的时长（毫秒）
    int currentDurationMs() const;

    // ---- 文件管理 ----

    // 列出所有录音文件（按时间倒序）
    QStringList listRecordings() const;

    // 录音目录总占用（字节）
    qint64 totalStorageBytes() const;

    // 删除一个录音文件。
    // 会校验路径确实落在录音目录内 —— 防止被传入任意路径而删掉用户的其他文件。
    RecorderResult deleteRecording(const QString &filePath);

    // 按保留天数清理旧录音。返回删除的文件数。
    int purgeOlderThan(int days);

    // 录音目录路径（用于在文件管理器中打开）
    QString outputDirectory() const;

    // ---- 裸音频数据访问 ----
    // 返回本次录音累积的 PCM 数据（仅在进行中的会话）。
    // 用途：按住说话场景下直接交给 SpeechService 识别，不必读文件。
    QByteArray capturedPcm() const;

signals:
    // 状态变化
    void stateChanged(dc::RecorderState state);

    // 录音开始（界面据此显示"录音中"指示，SRS 4.11 要求）
    void recordingStarted(const dc::RecordingSession &session);

    // 录音结束（正常停止或因超限自动停止）
    void recordingStopped(const dc::RecordingSession &session);

    // 录音过程中的进度（用于界面上显示已录时长/大小）
    void recordingProgress(int durationMs, qint64 bytesWritten);

    // 达到时长或空间上限，即将自动停止 —— 让界面能提前给出提示
    void limitReached(const QString &reason);

    // 发生需要用户注意的问题（写文件失败、设备异常等）
    void recorderError(const QString &message);

private:
    void onAudioDataReady();
    void tickProgress();
    bool openOutputFile(const QString &filePath);
    void closeOutputFile();
    bool writeWavHeader(QFile *file, quint32 dataBytes, const RecorderConfig &cfg) const;
    QString buildFileName() const;
    bool isPathInsideRecordingDirectory(const QString &path) const;
    void checkLimits();

    RecorderConfig m_config;
    RecorderState m_state = RecorderState::Idle;

    QString m_currentFilePath;
    QString m_currentSessionId;
    QDateTime m_startedAt;
    qint64 m_headerOffset = 0;         // WAV 头长度，停止时要回填大小
    qint64 m_bytesWritten = 0;
    bool m_autoStopped = false;
    QString m_stopReason;

    QByteArray m_pcmBuffer;            // 累积的 PCM（供识别使用）
    qint64 m_totalPcmBytes = 0;

    QFile *m_file = nullptr;
    QTimer *m_progressTimer = nullptr;

#ifdef DC_HAS_MULTIMEDIA
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioDevice = nullptr;
#endif
};

} // namespace dc

Q_DECLARE_METATYPE(dc::RecorderState)
Q_DECLARE_METATYPE(dc::RecordingSession)
