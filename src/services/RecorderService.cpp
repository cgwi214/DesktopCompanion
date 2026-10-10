#include "RecorderService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QUuid>

#ifdef DC_HAS_MULTIMEDIA
#  include <QAudioFormat>
#  include <QAudioSource>
#  include <QMediaDevices>
#  include <QAudioDevice>
#endif

#include <cstring>

namespace dc {

static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

QString recorderStateToString(RecorderState state)
{
    switch (state) {
    case RecorderState::Idle:        return QStringLiteral("idle");
    case RecorderState::Starting:    return QStringLiteral("starting");
    case RecorderState::Recording:   return QStringLiteral("recording");
    case RecorderState::Stopping:    return QStringLiteral("stopping");
    case RecorderState::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("idle");
}

// ============================================================================
// 构造
// ============================================================================
RecorderService::RecorderService(QObject *parent)
    : QObject(parent)
{
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(500);   // 半秒刷新一次进度足够
    connect(m_progressTimer, &QTimer::timeout, this, &RecorderService::tickProgress);
}

RecorderService::~RecorderService()
{
    // 析构时必须安全收尾：不能留下半个 WAV 文件。
    if (m_state == RecorderState::Recording || m_state == RecorderState::Starting) {
        DC_WARN(LogCat::Recording, QStringLiteral("RecorderService 析构时仍在录音，执行强制收尾"));
        stop(tr("程序退出"));
    }
    closeOutputFile();
}

// ============================================================================
// 配置
// ============================================================================
void RecorderService::applyConfig(const RecorderConfig &config)
{
    RecorderConfig effective = config;

    // 参数合法性收敛。这些值可能来自导入的配置文件，不能盲信。
    if (effective.sampleRate < 8000 || effective.sampleRate > 48000) {
        DC_WARN(LogCat::Recording, QStringLiteral("录音采样率 %1 超出合理范围，已改为 16000")
                    .arg(effective.sampleRate));
        effective.sampleRate = 16000;
    }
    if (effective.channels != 1 && effective.channels != 2) {
        effective.channels = 1;
    }
    if (effective.bitsPerSample != 8 && effective.bitsPerSample != 16) {
        effective.bitsPerSample = 16;
    }
    if (effective.maxDurationSeconds <= 0) {
        effective.maxDurationSeconds = 600;
    } else if (effective.maxDurationSeconds > 3600) {
        // 硬上限 1 小时。超过这个长度，无压缩 WAV 会有几百 MB，不合理。
        DC_WARN(LogCat::Recording, QStringLiteral("录音时长上限 %1 秒过大，已收敛为 3600 秒")
                    .arg(effective.maxDurationSeconds));
        effective.maxDurationSeconds = 3600;
    }
    if (effective.maxFileBytes <= 0) {
        effective.maxFileBytes = 50LL * 1024 * 1024;
    }
    if (effective.maxFileBytes > 2LL * 1024 * 1024 * 1024) {
        effective.maxFileBytes = 2LL * 1024 * 1024 * 1024;   // 上限 2GB
    }
    if (effective.maxTotalBytes <= 0) {
        effective.maxTotalBytes = 500LL * 1024 * 1024;
    }

    // 输出目录必须存在且合法
    if (!effective.outputDirectory.trimmed().isEmpty()) {
        QDir dir(effective.outputDirectory);
        if (!dir.exists()) {
            if (!dir.mkpath(QStringLiteral("."))) {
                DC_ERROR(LogCat::Recording,
                         QStringLiteral("无法创建录音输出目录"));
                effective.enabled = false;
            }
        }
    }

    const bool wasEnabled = m_config.enabled;
    m_config = effective;

    // 配置里把录音关掉了 -> 立即停止正在进行的录音（安全优先）
    if (wasEnabled && !m_config.enabled && isRecording()) {
        DC_INFO(LogCat::Recording, QStringLiteral("配置关闭了录音功能，正在停止进行中的录音"));
        stop(tr("录音功能已关闭"));
    }

    DC_INFO(LogCat::Recording, QStringLiteral("录音配置已应用：启用=%1 采样率=%2 时长上限=%3s 文件上限=%4MB")
                .arg(m_config.enabled ? QStringLiteral("是") : QStringLiteral("否"))
                .arg(m_config.sampleRate)
                .arg(m_config.maxDurationSeconds)
                .arg(m_config.maxFileBytes / (1024 * 1024)));
}

RecorderConfig RecorderService::config() const
{
    return m_config;
}

// ============================================================================
// 能力探测
//
//  分两层：
//    1. 编译期：是否有 Qt Multimedia（DC_HAS_MULTIMEDIA）
//    2. 运行时：系统里有没有可用的音频输入设备
//  两层都过才认为可用（SRS 6.2）。
// ============================================================================
bool RecorderService::probeCapability(QString *reason) const
{
#ifndef DC_HAS_MULTIMEDIA
    if (reason) {
        *reason = tr(
            "本程序编译时未启用音频采集支持，因此无法录音。"
            "（这通常是为了减小体积或因为构建环境缺少相应组件。）"
            "你仍然可以使用系统的录音机录制，再用本程序的语音功能。");
    }
    return false;
#else
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    if (inputs.isEmpty()) {
        if (reason) {
            *reason = tr("没有检测到可用的录音设备（麦克风）。"
                         "请检查设备是否已连接，以及系统是否允许本程序访问麦克风。");
        }
        return false;
    }

    if (reason) {
        reason->clear();
    }
    return true;
#endif
}

// ============================================================================
// 开关
// ============================================================================
void RecorderService::setEnabled(bool enabled)
{
    m_config.enabled = enabled;
    if (!enabled && isRecording()) {
        stop(tr("录音功能已关闭"));
    }
}

bool RecorderService::isEnabled() const
{
    return m_config.enabled;
}

// ============================================================================
// 状态
// ============================================================================
RecorderState RecorderService::state() const
{
    return m_state;
}

bool RecorderService::isRecording() const
{
    return m_state == RecorderState::Recording || m_state == RecorderState::Starting;
}

RecordingSession RecorderService::currentSession() const
{
    RecordingSession s;
    if (!isRecording()) {
        return s;
    }
    s.id = m_currentSessionId;
    s.filePath = m_currentFilePath;
    s.startedAt = m_startedAt;
    s.bytesWritten = m_bytesWritten;
    s.durationMs = currentDurationMs();
    return s;
}

int RecorderService::currentDurationMs() const
{
    if (m_startedAt.isNull()) {
        return 0;
    }
    return static_cast<int>(m_startedAt.msecsTo(QDateTime::currentDateTime()));
}

QByteArray RecorderService::capturedPcm() const
{
    return m_pcmBuffer;
}

// ============================================================================
// 开始录音
// ============================================================================
RecorderResult RecorderService::start()
{
    if (isRecording()) {
        return RecorderResult::failure(ec(erc::AlreadyExists),
                                       tr("已经在录音中。"));
    }

    // ---- 前置检查（每一项都给明确原因）----

    if (!m_config.enabled) {
        return RecorderResult::failure(
            ec(erc::RecordingDisabled),
            tr("录音功能处于关闭状态。请在设置中开启后再使用。"));
    }

    QString capReason;
    if (!probeCapability(&capReason)) {
        return RecorderResult::failure(ec(erc::RecordingDeviceError), capReason);
    }

    if (m_config.outputDirectory.trimmed().isEmpty()) {
        return RecorderResult::failure(
            ec(erc::InvalidArgument),
            tr("未配置录音保存位置。"));
    }

    // 目录空间检查：剩余空间不足 2 倍文件上限就别开始了，
    // 免得录到一半失败，用户白等。
    {
        QDir dir(m_config.outputDirectory);
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            return RecorderResult::failure(
                ec(erc::RecordingWriteFailed),
                tr("无法创建录音保存目录：%1").arg(m_config.outputDirectory));
        }

        const QFileInfo dirInfo(m_config.outputDirectory);
        // 注意：这只是一个粗略估计，某些文件系统不报告可用空间
        const qint64 available = 0;   // 见下方说明
        Q_UNUSED(available)
        // 说明：Qt 没有跨平台的"磁盘可用空间"API。
        // 这里不做磁盘空间预检，而是依赖写入失败时的错误处理 ——
        // 写失败时会有明确提示，且录音文件会被清理。
        Q_UNUSED(dirInfo)
    }

    // ---- 打开输出文件 ----
    const QString filePath = QDir(m_config.outputDirectory).filePath(buildFileName());
    if (!openOutputFile(filePath)) {
        return RecorderResult::failure(
            ec(erc::RecordingWriteFailed),
            tr("无法创建录音文件：%1").arg(QFileInfo(filePath).fileName()));
    }

    // ---- 状态置位 ----
    m_state = RecorderState::Starting;
    m_currentFilePath = filePath;
    m_currentSessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_startedAt = QDateTime::currentDateTime();
    m_bytesWritten = 0;
    m_autoStopped = false;
    m_stopReason.clear();
    m_pcmBuffer.clear();
    m_totalPcmBytes = 0;

#ifdef DC_HAS_MULTIMEDIA
    // ---- 配置音频格式 ----
    QAudioFormat format;
    format.setSampleRate(m_config.sampleRate);
    format.setChannelCount(m_config.channels);
    format.setSampleFormat(m_config.bitsPerSample == 16
                               ? QAudioFormat::Int16
                               : QAudioFormat::UInt8);

    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (!device.isFormatSupported(format)) {
        // 设备不支持我们想要的格式 —— 不强行用相近格式（那样录出来的
        // 数据格式与文件头不一致，会变成噪音）。直接如实失败。
        const QAudioFormat preferred = device.preferredFormat();
        DC_WARN(LogCat::Recording, QStringLiteral("录音设备不支持 %1Hz/%2ch，首选为 %3Hz/%4ch")
                    .arg(m_config.sampleRate).arg(m_config.channels)
                    .arg(preferred.sampleRate()).arg(preferred.channelCount()));

        // 尝试用设备的首选格式，但要相应更新文件头参数，
        // 否则 WAV 头与实际数据不符，播放器会报错。
        RecorderConfig fallback = m_config;
        if (preferred.sampleRate() > 0) {
            fallback.sampleRate = preferred.sampleRate();
        }
        if (preferred.channelCount() > 0) {
            fallback.channels = preferred.channelCount();
        }
        fallback.bitsPerSample = (preferred.sampleFormat() == QAudioFormat::Int16) ? 16 : 8;

        if (!device.isFormatSupported(QAudioFormat{})) {
            // 连回退格式也不行
            closeOutputFile();
            m_state = RecorderState::Unavailable;
            emit stateChanged(m_state);
            return RecorderResult::failure(
                ec(erc::RecordingDeviceError),
                tr("录音设备不支持所需的音频格式。"));
        }

        DC_INFO(LogCat::Recording, QStringLiteral("已改用设备首选格式：%1Hz/%2ch")
                    .arg(fallback.sampleRate).arg(fallback.channels));
        m_config.sampleRate = fallback.sampleRate;
        m_config.channels = fallback.channels;
        m_config.bitsPerSample = fallback.bitsPerSample;

        format.setSampleRate(m_config.sampleRate);
        format.setChannelCount(m_config.channels);
        format.setSampleFormat(m_config.bitsPerSample == 16
                                   ? QAudioFormat::Int16
                                   : QAudioFormat::UInt8);
    }

    // 先写入 WAV 头（大小待回填）
    if (!writeWavHeader(m_file, 0, m_config)) {
        closeOutputFile();
        m_state = RecorderState::Idle;
        emit stateChanged(m_state);
        return RecorderResult::failure(ec(erc::RecordingWriteFailed),
                                       tr("无法写入录音文件头。"));
    }
    m_headerOffset = m_bytesWritten;

    // ---- 启动采集 ----
    m_audioSource = new QAudioSource(device, format, this);
    m_audioDevice = m_audioSource->start();

    if (!m_audioDevice) {
        closeOutputFile();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_state = RecorderState::Unavailable;
        emit stateChanged(m_state);
        return RecorderResult::failure(
            ec(erc::RecordingDeviceError),
            tr("无法启动录音设备。可能被其他程序占用，或系统未授予麦克风权限。"));
    }

    connect(m_audioDevice, &QIODevice::readyRead, this, &RecorderService::onAudioDataReady);
    connect(m_audioSource, &QAudioSource::stateChanged, this,
            [this](QAudio::State audioState) {
                // 设备被拔掉/被抢占 -> 主动收尾，不要留下一个坏文件
                if (audioState == QAudio::StoppedState && m_state == RecorderState::Recording) {
                    if (m_audioSource && m_audioSource->error() != QAudio::NoError) {
                        DC_WARN(LogCat::Recording, QStringLiteral("录音设备异常停止（error=%1）")
                                    .arg(static_cast<int>(m_audioSource->error())));
                        emit recorderError(tr("录音设备异常，录音已停止。"));
                        stop(tr("录音设备异常"));
                    }
                }
            });
#else
    closeOutputFile();
    m_state = RecorderState::Unavailable;
    emit stateChanged(m_state);
    return RecorderResult::failure(
        ec(erc::RecordingDeviceError),
        tr("本程序编译时未启用音频采集支持，无法录音。"));
#endif

    m_state = RecorderState::Recording;
    m_progressTimer->start();

    emit stateChanged(m_state);
    emit recordingStarted(currentSession());

    DC_INFO(LogCat::Recording, QStringLiteral("录音已开始：%1（%2Hz/%3ch，上限 %4s）")
                .arg(QFileInfo(filePath).fileName())
                .arg(m_config.sampleRate)
                .arg(m_config.channels)
                .arg(m_config.maxDurationSeconds));

    return RecorderResult::success(currentSession());
}

// ============================================================================
// 停止录音
// ============================================================================
RecorderResult RecorderService::stop(const QString &stopReason)
{
    if (!isRecording()) {
        return RecorderResult::failure(ec(erc::RecordingNotStarted),
                                       tr("当前没有正在进行的录音。"));
    }

    m_state = RecorderState::Stopping;
    emit stateChanged(m_state);

    m_progressTimer->stop();

#ifdef DC_HAS_MULTIMEDIA
    if (m_audioSource) {
        // stop() 会触发最后一次 readyRead，必须先把剩余数据读完再关文件，
        // 否则最后几百毫秒的录音会丢失。
        if (m_audioDevice) {
            onAudioDataReady();
        }
        m_audioSource->stop();
        delete m_audioSource;
        m_audioSource = nullptr;
        m_audioDevice = nullptr;
    }
#endif

    // ---- 回填 WAV 头里的大小字段 ----
    const quint32 dataBytes = static_cast<quint32>(m_bytesWritten - m_headerOffset);
    if (m_file && m_file->isOpen() && m_headerOffset > 0) {
        m_file->seek(0);
        // 头已经在 openOutputFile 里占了位置，这里重写完整头
        writeWavHeader(m_file, dataBytes, m_config);
        m_file->seek(m_file->size());
    }

    closeOutputFile();

    RecordingSession session;
    session.id = m_currentSessionId;
    session.filePath = m_currentFilePath;
    session.startedAt = m_startedAt;
    session.stoppedAt = QDateTime::currentDateTime();
    session.bytesWritten = m_bytesWritten;
    session.durationMs = static_cast<int>(m_startedAt.msecsTo(session.stoppedAt));
    session.autoStopped = m_autoStopped;
    session.stopReason = stopReason.isEmpty() ? m_stopReason : stopReason;

    const qint64 fileSize = QFileInfo(m_currentFilePath).size();
    const bool fileOk = fileSize > m_headerOffset;

    m_state = RecorderState::Idle;
    m_currentFilePath.clear();
    m_currentSessionId.clear();
    m_startedAt = QDateTime();

    emit stateChanged(m_state);

    if (!fileOk) {
        // 文件是空的或只有头 —— 说明采集根本没成功。
        // 删除这个无用的空文件，不要让用户在列表里看到它。
        QFile::remove(session.filePath);
        DC_WARN(LogCat::Recording, QStringLiteral("录音文件为空，已删除"));
        return RecorderResult::failure(
            ec(erc::RecordingWriteFailed),
            tr("没有录到音频数据，录音文件已被删除。"
               "请检查麦克风是否可用。"));
    }

    emit recordingStopped(session);

    DC_INFO(LogCat::Recording, QStringLiteral("录音已停止：时长 %1ms，大小 %2KB%3")
                .arg(session.durationMs)
                .arg(fileSize / 1024)
                .arg(session.autoStopped
                         ? QStringLiteral("（因超限自动停止：%1）").arg(session.stopReason)
                         : QString()));

    return RecorderResult::success(session);
}

// ============================================================================
// 数据回调
// ============================================================================
void RecorderService::onAudioDataReady()
{
#ifdef DC_HAS_MULTIMEDIA
    if (!m_audioDevice || !m_file || !m_file->isOpen()) {
        return;
    }

    const QByteArray chunk = m_audioDevice->readAll();
    if (chunk.isEmpty()) {
        return;
    }

    // 空间上限检查（在写入之前判断，避免写超）
    if (m_bytesWritten + chunk.size() > m_headerOffset + m_config.maxFileBytes) {
        // 只写进去能写下的部分，然后停止
        const qint64 allowed = qMax<qint64>(
            0, m_headerOffset + m_config.maxFileBytes - m_bytesWritten);
        if (allowed > 0) {
            const QByteArray partial = chunk.left(static_cast<int>(allowed));
            m_file->write(partial);
            m_bytesWritten += partial.size();
        }

        m_autoStopped = true;
        m_stopReason = tr("已达到单次录音的大小上限（%1 MB）")
                           .arg(m_config.maxFileBytes / (1024 * 1024));
        emit limitReached(m_stopReason);

        // 延后一拍停止，避免在信号回调里直接改状态
        QTimer::singleShot(0, this, [this]() {
            if (isRecording()) {
                stop(m_stopReason);
            }
        });
        return;
    }

    const qint64 written = m_file->write(chunk);
    if (written < 0) {
        DC_ERROR(LogCat::Recording, QStringLiteral("录音文件写入失败"));
        emit recorderError(tr("录音数据写入失败，磁盘可能已满或没有写入权限。"));
        QTimer::singleShot(0, this, [this]() {
            if (isRecording()) {
                stop(tr("写入失败"));
            }
        });
        return;
    }

    m_bytesWritten += written;

    // 累积 PCM 供识别使用。但要有上限：
    // 完整录 10 分钟是 19MB，长期累积在内存里不合理。
    // 识别只需要最后几秒的内容（按住说话场景），所以保留尾部即可。
    static constexpr int kMaxPcmBufferBytes = 16000 * 2 * 30;   // 约 30 秒
    m_pcmBuffer.append(chunk);
    m_totalPcmBytes += chunk.size();
    if (m_pcmBuffer.size() > kMaxPcmBufferBytes) {
        m_pcmBuffer = m_pcmBuffer.right(kMaxPcmBufferBytes);
    }
#endif
}

// ============================================================================
// 进度与上限检查
// ============================================================================
void RecorderService::tickProgress()
{
    if (!isRecording()) {
        return;
    }

    const int durationMs = currentDurationMs();
    emit recordingProgress(durationMs, m_bytesWritten);

    checkLimits();
}

void RecorderService::checkLimits()
{
    if (!isRecording()) {
        return;
    }

    // 时长上限
    if (currentDurationMs() >= m_config.maxDurationSeconds * 1000) {
        m_autoStopped = true;
        m_stopReason = tr("已达到单次录音的时长上限（%1 分钟）")
                           .arg(m_config.maxDurationSeconds / 60.0, 0, 'f', 1);
        DC_INFO(LogCat::Recording, QStringLiteral("录音达到时长上限，自动停止"));
        emit limitReached(m_stopReason);
        stop(m_stopReason);
        return;
    }

    // 提前 30 秒预警，让界面能提示用户"快结束了"
    static constexpr int kWarnBeforeSeconds = 30;
    const int remainingSeconds =
        m_config.maxDurationSeconds - currentDurationMs() / 1000;
    if (remainingSeconds == kWarnBeforeSeconds) {
        emit limitReached(tr("录音还剩 %1 秒即将自动停止。").arg(kWarnBeforeSeconds));
    }
}

// ============================================================================
// 文件操作
// ============================================================================
bool RecorderService::openOutputFile(const QString &filePath)
{
    closeOutputFile();

    m_file = new QFile(filePath, this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        DC_ERROR(LogCat::Recording,
                 QStringLiteral("无法打开录音文件用于写入"));
        delete m_file;
        m_file = nullptr;
        return false;
    }

    // 先占出 WAV 头的空间，停止时回填
    m_bytesWritten = m_file->write(QByteArray(44, '\0'));
    if (m_bytesWritten != 44) {
        closeOutputFile();
        return false;
    }

    return true;
}

void RecorderService::closeOutputFile()
{
    if (m_file) {
        if (m_file->isOpen()) {
            m_file->flush();
            m_file->close();
        }
        m_file->deleteLater();
        m_file = nullptr;
    }
    m_headerOffset = 0;
}

// ---------------------------------------------------------------------------
// 写 WAV 头
//
//  这是标准的 44 字节 RIFF/WAVE 头。手写而不是用库，
//  因为这是唯一需要写二进制音频格式的地方，引入库不划算。
// ---------------------------------------------------------------------------
bool RecorderService::writeWavHeader(QFile *file, quint32 dataBytes,
                                     const RecorderConfig &cfg) const
{
    if (!file || !file->isOpen()) {
        return false;
    }

    const quint16 channels = static_cast<quint16>(cfg.channels);
    const quint32 sampleRate = static_cast<quint32>(cfg.sampleRate);
    const quint16 bitsPerSample = static_cast<quint16>(cfg.bitsPerSample);
    const quint16 blockAlign =
        static_cast<quint16>(channels * bitsPerSample / 8);
    const quint32 byteRate = sampleRate * blockAlign;

    QByteArray header;
    header.reserve(44);

    auto appendU32 = [&header](quint32 v) {
        header.append(static_cast<char>(v & 0xFF));
        header.append(static_cast<char>((v >> 8) & 0xFF));
        header.append(static_cast<char>((v >> 16) & 0xFF));
        header.append(static_cast<char>((v >> 24) & 0xFF));
    };
    auto appendU16 = [&header](quint16 v) {
        header.append(static_cast<char>(v & 0xFF));
        header.append(static_cast<char>((v >> 8) & 0xFF));
    };
    auto appendTag = [&header](const char *tag) {
        header.append(tag, 4);
    };

    // RIFF chunk descriptor
    appendTag("RIFF");
    // 整个文件大小减去前 8 字节
    appendU32(36 + dataBytes);
    appendTag("WAVE");

    // fmt sub-chunk
    appendTag("fmt ");
    appendU32(16);                    // PCM 格式的 fmt 块长度固定 16
    appendU16(1);                     // AudioFormat = 1 表示未压缩 PCM
    appendU16(channels);
    appendU32(sampleRate);
    appendU32(byteRate);
    appendU16(blockAlign);
    appendU16(bitsPerSample);

    // data sub-chunk
    appendTag("data");
    appendU32(dataBytes);

    if (header.size() != 44) {
        DC_ERROR(LogCat::Recording,
                 QStringLiteral("WAV 头长度异常：%1").arg(header.size()));
        return false;
    }

    const qint64 written = file->write(header);
    return written == 44;
}

QString RecorderService::buildFileName() const
{
    // 文件名格式：recording_20260925_203712_a1b2c3.wav
    // 带时间戳便于用户按时间找，带短随机串避免同一秒内冲突。
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString unique = QUuid::createUuid()
                               .toString(QUuid::WithoutBraces)
                               .left(6);
    return QStringLiteral("%1_%2_%3.wav")
        .arg(m_config.filePrefix, stamp, unique);
}

// ---------------------------------------------------------------------------
// 路径归属检查
//
//  deleteRecording 会被界面传入路径。必须确认它确实在录音目录里，
//  否则一个有 bug 的调用点就能删掉用户的任意文件。
//  这里做两层检查：字符串级别的前缀匹配 + 规范化后的真实路径比较。
// ---------------------------------------------------------------------------
bool RecorderService::isPathInsideRecordingDirectory(const QString &path) const
{
    if (m_config.outputDirectory.trimmed().isEmpty() || path.trimmed().isEmpty()) {
        return false;
    }

    const QString canonicalDir =
        QFileInfo(m_config.outputDirectory).canonicalFilePath();
    const QString canonicalFile = QFileInfo(path).canonicalFilePath();

    if (canonicalDir.isEmpty() || canonicalFile.isEmpty()) {
        // 文件可能已被删除，退回字符串比较
        const QString absDir = QDir(m_config.outputDirectory).absolutePath();
        const QString absFile = QFileInfo(path).absolutePath();
        return absFile == absDir;
    }

    // 加分隔符比较，避免 "/rec" 匹配到 "/recordings-other"
    const QString dirWithSep = canonicalDir.endsWith(QLatin1Char('/'))
                                   ? canonicalDir
                                   : canonicalDir + QLatin1Char('/');
    return canonicalFile.startsWith(dirWithSep);
}

// ============================================================================
// 文件列表与清理
// ============================================================================
QStringList RecorderService::listRecordings() const
{
    QStringList result;
    if (m_config.outputDirectory.trimmed().isEmpty()) {
        return result;
    }

    QDir dir(m_config.outputDirectory);
    if (!dir.exists()) {
        return result;
    }

    const QStringList filters{ QStringLiteral("*.wav") };
    const QFileInfoList entries =
        dir.entryInfoList(filters, QDir::Files | QDir::Readable, QDir::Time);

    for (const QFileInfo &info : entries) {
        result << info.absoluteFilePath();
    }
    return result;
}

qint64 RecorderService::totalStorageBytes() const
{
    qint64 total = 0;
    if (m_config.outputDirectory.trimmed().isEmpty()) {
        return total;
    }

    QDir dir(m_config.outputDirectory);
    if (!dir.exists()) {
        return total;
    }

    const QFileInfoList entries =
        dir.entryInfoList({ QStringLiteral("*.wav") }, QDir::Files | QDir::Readable);
    for (const QFileInfo &info : entries) {
        total += info.size();
    }
    return total;
}

RecorderResult RecorderService::deleteRecording(const QString &filePath)
{
    if (!isPathInsideRecordingDirectory(filePath)) {
        DC_WARN(LogCat::Recording, QStringLiteral("拒绝删除录音目录之外的路径：%1")
                    .arg(PathGuard::redact(filePath)));
        return RecorderResult::failure(
            ec(erc::PathNotAllowed),
            tr("只能删除录音目录内的文件。"));
    }

    // 不允许删除正在录制的那个文件
    if (isRecording() && !m_currentFilePath.isEmpty()
        && QFileInfo(m_currentFilePath).absoluteFilePath()
               == QFileInfo(filePath).absoluteFilePath()) {
        return RecorderResult::failure(
            ec(erc::RecordingNotStarted),
            tr("该文件正在录制中，无法删除。请先停止录音。"));
    }

    QFile file(filePath);
    if (!file.exists()) {
        return RecorderResult::failure(ec(erc::PathNotFound),
                                       tr("录音文件不存在。"));
    }

    if (!file.remove()) {
        return RecorderResult::failure(
            ec(erc::RecordingWriteFailed),
            tr("删除录音文件失败：%1").arg(file.errorString()));
    }

    DC_INFO(LogCat::Recording, QStringLiteral("已删除录音文件"));
    return RecorderResult::success();
}

int RecorderService::purgeOlderThan(int days)
{
    if (days < 0) {
        return 0;
    }

    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    const QStringList files = listRecordings();
    int removed = 0;

    for (const QString &path : files) {
        const QFileInfo info(path);
        if (info.lastModified() < cutoff) {
            const RecorderResult r = deleteRecording(path);
            if (r.ok) {
                ++removed;
            }
        }
    }

    if (removed > 0) {
        DC_INFO(LogCat::Recording, QStringLiteral("清理了 %1 个超过 %2 天的录音文件").arg(removed).arg(days));
    }
    return removed;
}

QString RecorderService::outputDirectory() const
{
    return m_config.outputDirectory;
}

} // namespace dc
