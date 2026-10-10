// ============================================================================
//  Logger.h — 分级日志、轮转与敏感信息脱敏
//
//  需求依据：SRS V1.1
//    7.2 稳定性：日志应支持级别控制、轮转和清理
//    5   配置要求：不在日志中输出 API 密钥、录音内容或完整敏感路径
//    9.2 安全测试：API 密钥和敏感数据不得进入普通日志
//
//  设计说明：
//    脱敏不是「调用方自觉」，而是日志器的强制职责：
//      任何进入 Logger 的字符串都会经过 sanitize()。
//    这样即使某个模块不小心把密钥写进日志参数，也不会真正落盘。
// ============================================================================
#pragma once

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTextStream>

namespace dc {

enum class LogLevel {
    Trace = 0,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
    Off
};

QString logLevelToString(LogLevel level);
LogLevel logLevelFromString(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------------------
// 日志类别：便于按模块过滤，且能让用户明确知道某条日志来自哪个模块
// ---------------------------------------------------------------------------
namespace LogCat {
inline const char *Core      = "core";
inline const char *Config    = "config";
inline const char *Command   = "command";
inline const char *Permission= "permission";
inline const char *System    = "system";
inline const char *Network   = "network";
inline const char *Search    = "search";
inline const char *Translate = "translate";
inline const char *History   = "history";
inline const char *Speech    = "speech";
inline const char *Recording = "recording";
inline const char *Ui        = "ui";
inline const char *Platform  = "platform";
} // namespace LogCat

// ---------------------------------------------------------------------------
// 日志配置
// ---------------------------------------------------------------------------
struct LogConfig {
    LogLevel level = LogLevel::Info;
    QString directory;                      // 日志目录
    QString baseFileName = QStringLiteral("desktop_companion");
    qint64 maxFileSizeBytes = 2LL * 1024 * 1024;   // 单文件上限 2 MB
    int maxBackupFiles = 5;                 // 保留的历史文件数（轮转）
    int retentionDays = 30;                 // 超过天数自动清理
    bool consoleOutput = false;             // 是否同时输出到 stderr（开发期用）
};

// ---------------------------------------------------------------------------
// Logger —— 进程级单例
//   线程安全：内部 QMutex 串行化写入，SRS 7.1 要求网络/任务异步执行，
//             因此日志必然被多线程调用。
// ---------------------------------------------------------------------------
class Logger
{
public:
    static Logger &instance();

    // 初始化：创建目录、打开文件。失败时不抛异常，只降级为「仅内存记录」，
    // 保证核心功能不因日志不可写而无法启动（SRS 7.2）。
    bool initialize(const LogConfig &config);
    void shutdown();

    LogConfig config() const;
    void setLevel(LogLevel level);
    LogLevel level() const;

    void log(LogLevel level, const char *category, const QString &message);

    // 清理过期日志（SRS 7.2：日志轮转和清理）
    int purgeExpired();

    // ---------------------------------------------------------------------
    // 脱敏规则（公开以便测试直接验证，见 tests/unit/tst_logger.cpp）
    // ---------------------------------------------------------------------
    // 覆盖：API key / token / password / secret / 长十六进制串 / 长路径
    static QString sanitize(const QString &text);

    // 供测试使用的内存日志（不落盘）
    QStringList capturedLines() const;
    void clearCaptured();

private:
    Logger() = default;
    Q_DISABLE_COPY(Logger)

    void rotateIfNeeded();
    void writeLine(const QString &line);

    mutable QMutex m_mutex;
    LogConfig m_config;
    QFile m_file;
    QTextStream m_stream;
    bool m_initialized = false;
    QStringList m_captured;   // 仅测试用
};

// ---------------------------------------------------------------------------
// 便捷宏：自动带上文件/行号，便于定位；消息统一经过 Logger::sanitize
// ---------------------------------------------------------------------------
#define DC_LOG(level, category, message)                                        \
    do {                                                                        \
        ::dc::Logger::instance().log((level), (category),                       \
            QStringLiteral("%1 (%2:%3)")                                        \
                .arg(QString(message))                                          \
                .arg(QString::fromUtf8(__FILE__))                               \
                .arg(__LINE__));                                                \
    } while (false)

#define DC_TRACE(cat, msg)  DC_LOG(::dc::LogLevel::Trace,   cat, msg)
#define DC_DEBUG(cat, msg)  DC_LOG(::dc::LogLevel::Debug,   cat, msg)
#define DC_INFO(cat, msg)   DC_LOG(::dc::LogLevel::Info,    cat, msg)
#define DC_WARN(cat, msg)   DC_LOG(::dc::LogLevel::Warning, cat, msg)
#define DC_ERROR(cat, msg)  DC_LOG(::dc::LogLevel::Error,   cat, msg)
#define DC_FATAL(cat, msg)  DC_LOG(::dc::LogLevel::Critical,cat, msg)

} // namespace dc
