// ============================================================================
//  Logger.cpp — 实现分级日志、轮转、脱敏
//
//  需求依据：SRS V1.1 5、7.2、7.3、9.2
// ============================================================================
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QThread>

namespace dc {

// ===========================================================================
//  LogLevel 互转
// ===========================================================================
QString logLevelToString(LogLevel level)
{
    switch (level) {
    case LogLevel::Trace:    return QStringLiteral("TRACE");
    case LogLevel::Debug:    return QStringLiteral("DEBUG");
    case LogLevel::Info:     return QStringLiteral("INFO");
    case LogLevel::Warning:  return QStringLiteral("WARN");
    case LogLevel::Error:    return QStringLiteral("ERROR");
    case LogLevel::Critical: return QStringLiteral("FATAL");
    case LogLevel::Off:      return QStringLiteral("OFF");
    }
    return QStringLiteral("INFO");
}

LogLevel logLevelFromString(const QString &text, bool *ok)
{
    const QString key = text.trimmed().toLower();
    if (key == QStringLiteral("trace"))   { if (ok) *ok = true; return LogLevel::Trace; }
    if (key == QStringLiteral("debug"))   { if (ok) *ok = true; return LogLevel::Debug; }
    if (key == QStringLiteral("info"))    { if (ok) *ok = true; return LogLevel::Info; }
    if (key == QStringLiteral("warn") ||
        key == QStringLiteral("warning")) { if (ok) *ok = true; return LogLevel::Warning; }
    if (key == QStringLiteral("error"))   { if (ok) *ok = true; return LogLevel::Error; }
    if (key == QStringLiteral("critical") ||
        key == QStringLiteral("fatal"))   { if (ok) *ok = true; return LogLevel::Critical; }
    if (key == QStringLiteral("off"))     { if (ok) *ok = true; return LogLevel::Off; }

    if (ok) *ok = false;
    return LogLevel::Info;
}

// ===========================================================================
//  脱敏规则
//
//  这是 SRS 7.3 / 9.2 的强制要求落点。规则分三类：
//    (A) 键值型密钥：api_key=xxx、token: xxx、password="xxx" 等
//    (B) Bearer / Basic 认证头
//    (C) 过长的高熵字符串（疑似密钥或哈希）
//    (D) 用户主目录绝对路径（避免日志泄露完整敏感路径）
// ===========================================================================
namespace {

struct SanitizeRule {
    QRegularExpression pattern;
    QString replacement;
};

const QVector<SanitizeRule> &sanitizeRules()
{
    static const QVector<SanitizeRule> rules = [] {
        QVector<SanitizeRule> r;
        const QRegularExpression::PatternOptions ci =
            QRegularExpression::CaseInsensitiveOption;

        // (A) 键值型密钥。保留键名以便排错，只抹掉值。
        //     支持引号包裹和裸露两种写法。
        r.append({ QRegularExpression(
                       QStringLiteral(
                           "(\\b(?:api[_-]?key|apikey|access[_-]?token|refresh[_-]?token|"
                           "auth[_-]?token|secret|client[_-]?secret|password|passwd|pwd|"
                           "credential|private[_-]?key)\\b\\s*[:=]\\s*)"
                           "(\"[^\"]*\"|'[^']*'|[^\\s,;&\\}\\)]+)"),
                       ci),
                   QStringLiteral("\\1**REDACTED**") });

        // (B) HTTP 认证头
        r.append({ QRegularExpression(
                       QStringLiteral("(\\b(?:Bearer|Basic|Token)\\s+)[A-Za-z0-9\\-._~+/=]{8,}"),
                       ci),
                   QStringLiteral("\\1**REDACTED**") });

        // (C) 疑似高熵密钥：长度 >= 24 且不含空格的连续字母数字串，
        //     同时要求包含大小写或数字，降低误伤普通单词的概率。
        r.append({ QRegularExpression(
                       QStringLiteral("\\b(?=[A-Za-z0-9_\\-]{24,}\\b)"
                                      "(?=[^\\s]*[A-Z])(?=[^\\s]*[a-z])"
                                      "(?=[^\\s]*[0-9])[A-Za-z0-9_\\-]{24,}\\b")),
                   QStringLiteral("**REDACTED_TOKEN**") });

        // (D) 用户主目录绝对路径：Windows "C:\Users\xxx\..." 与 POSIX "/home/xxx/..."
        //     只保留盘符/根与最后一级，中间用 ~ 代替。
        r.append({ QRegularExpression(
                       QStringLiteral("([A-Za-z]:\\\\Users\\\\)[^\\\\\\s]+"),
                       ci),
                   QStringLiteral("\\1<user>") });
        r.append({ QRegularExpression(
                       QStringLiteral("(/home/)[^/\\s]+"),
                       ci),
                   QStringLiteral("\\1<user>") });

        return r;
    }();
    return rules;
}

} // namespace

QString Logger::sanitize(const QString &text)
{
    QString out = text;
    for (const SanitizeRule &rule : sanitizeRules())
        out.replace(rule.pattern, rule.replacement);
    return out;
}

// ===========================================================================
//  Logger
// ===========================================================================
Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

bool Logger::initialize(const LogConfig &config)
{
    QMutexLocker locker(&m_mutex);

    m_config = config;

    if (m_config.directory.isEmpty())
        m_config.directory = QDir::currentPath();

    QDir dir(m_config.directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        // 目录建不出来 -> 降级为仅内存记录，绝不抛出（SRS 7.2）
        m_initialized = false;
        m_captured.append(QStringLiteral("[Logger] 无法创建日志目录：%1，日志将不落盘")
                              .arg(m_config.directory));
        return false;
    }

    const QString path = dir.absoluteFilePath(m_config.baseFileName + QStringLiteral(".log"));
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_initialized = false;
        m_captured.append(QStringLiteral("[Logger] 无法打开日志文件：%1").arg(path));
        return false;
    }

    m_stream.setDevice(&m_file);
    m_stream.setEncoding(QStringConverter::Utf8);
    m_initialized = true;
    return true;
}

void Logger::shutdown()
{
    QMutexLocker locker(&m_mutex);
    if (m_file.isOpen()) {
        m_stream.flush();
        m_file.close();
    }
    m_initialized = false;
}

LogConfig Logger::config() const
{
    QMutexLocker locker(&m_mutex);
    return m_config;
}

void Logger::setLevel(LogLevel level)
{
    QMutexLocker locker(&m_mutex);
    m_config.level = level;
}

LogLevel Logger::level() const
{
    QMutexLocker locker(&m_mutex);
    return m_config.level;
}

void Logger::log(LogLevel level, const char *category, const QString &message)
{
    QMutexLocker locker(&m_mutex);

    if (level < m_config.level || m_config.level == LogLevel::Off)
        return;

    const QString safeMessage = sanitize(message);
    const QString cat = QString::fromUtf8(category ? category : "general");

    // 日志级别列固定 7 字符宽，用左对齐补空格。
    //
    // 这里踩过一个坑：最早写的是 printf 风格的 "[%-6s]"，但 QString::arg()
    // 不认 printf 占位符，于是 "%-6s" 被原样打进日志文件，看着像乱码。
    // QString 的对齐语法是 arg(value, fieldWidth, fillChar)，注意第三个参数
    // 是"填充字符"而不是像 printf 那样用 '-' 表示左对齐 —— 这一点很容易写错。
    const QString levelTag = logLevelToString(level).leftJustified(7, QLatin1Char(' '));

    const QString line = QStringLiteral("%1 [%2] [%3] [t:%4] %5")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             cat,
             levelTag)
        .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16)
        .arg(safeMessage);

    writeLine(line);
}

void Logger::writeLine(const QString &line)
{
    // 内存副本：测试用，同时也是日志文件不可写时的兜底
    m_captured.append(line);
    if (m_captured.size() > 5000)
        m_captured.removeFirst();

    if (!m_initialized)
        return;

    rotateIfNeeded();

    m_stream << line << '\n';
    m_stream.flush();

    if (m_config.consoleOutput)
        qDebug().noquote() << line;
}

void Logger::rotateIfNeeded()
{
    if (!m_file.isOpen())
        return;

    if (m_config.maxFileSizeBytes <= 0)
        return;

    if (m_stream.device()->pos() < m_config.maxFileSizeBytes)
        return;

    const QString path = m_file.fileName();
    m_stream.flush();
    m_file.close();

    // 轮转：base.log -> base.1.log -> base.2.log ... 超出上限的丢弃
    const QString base = QFileInfo(path).completeBaseName();
    const QString dirPath = QFileInfo(path).absolutePath();

    if (m_config.maxBackupFiles > 0) {
        // 先把最老的删掉
        const QString oldest = QDir(dirPath).absoluteFilePath(
            QStringLiteral("%1.%2.log").arg(base).arg(m_config.maxBackupFiles));
        if (QFile::exists(oldest))
            QFile::remove(oldest);

        // 依次后移
        for (int i = m_config.maxBackupFiles - 1; i >= 1; --i) {
            const QString from = QDir(dirPath).absoluteFilePath(
                QStringLiteral("%1.%2.log").arg(base).arg(i));
            const QString to = QDir(dirPath).absoluteFilePath(
                QStringLiteral("%1.%2.log").arg(base).arg(i + 1));
            if (QFile::exists(from)) {
                if (QFile::exists(to))
                    QFile::remove(to);
                QFile::rename(from, to);
            }
        }

        // 当前文件改名为 .1.log
        const QString first = QDir(dirPath).absoluteFilePath(
            QStringLiteral("%1.1.log").arg(base));
        if (QFile::exists(first))
            QFile::remove(first);
        QFile::rename(path, first);
    } else {
        // 不保留备份 -> 直接截断
        QFile::remove(path);
    }

    m_file.setFileName(path);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_stream.setEncoding(QStringConverter::Utf8);
    } else {
        // 重开失败 -> 关闭落盘，转内存模式，保证程序继续运行（SRS 7.2）
        m_initialized = false;
    }
}

int Logger::purgeExpired()
{
    QMutexLocker locker(&m_mutex);

    if (m_config.retentionDays <= 0 || m_config.directory.isEmpty())
        return 0;

    QDir dir(m_config.directory);
    const QStringList filters{ m_config.baseFileName + QStringLiteral("*.log") };

    const QDateTime deadline =
        QDateTime::currentDateTime().addDays(-m_config.retentionDays);

    int removed = 0;
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Time);
    for (const QFileInfo &info : entries) {
        if (info.lastModified() < deadline) {
            if (QFile::remove(info.absoluteFilePath()))
                ++removed;
        }
    }
    return removed;
}

QStringList Logger::capturedLines() const
{
    QMutexLocker locker(&m_mutex);
    return m_captured;
}

void Logger::clearCaptured()
{
    QMutexLocker locker(&m_mutex);
    m_captured.clear();
}

} // namespace dc
