// ============================================================================
//  PlatformAdapter.cpp — 工厂与共享实现
//
//  需求依据：SRS V1.1 3.2、4.3、6.2、7.3
// ============================================================================
#include "platform/PlatformAdapter.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace dc {

namespace {
// 单例。故意不加锁：只在启动阶段（单线程）装配一次，
// 之后全程只读。运行期重新装配属于编程错误，用断言暴露。
PlatformAdapter *g_adapter = nullptr;
} // namespace

PlatformAdapter::PlatformAdapter(QObject *parent)
    : QObject(parent)
{
}

PlatformAdapter::~PlatformAdapter()
{
    if (g_adapter == this) {
        g_adapter = nullptr;
    }
}

PlatformAdapter *PlatformAdapter::instance()
{
    return g_adapter;
}

PlatformAdapter *PlatformAdapter::createInstance(QObject *parent)
{
    if (g_adapter) {
        delete g_adapter;
        g_adapter = nullptr;
    }
    g_adapter = create(parent);
    return g_adapter;
}

// ---------------------------------------------------------------------------
// 工厂
//  这里出现的是「唯一的编译期平台分支」。业务代码永远看不到这些宏。
// ---------------------------------------------------------------------------
PlatformAdapter *PlatformAdapter::create(QObject *parent)
{
#ifdef Q_OS_WIN
    extern PlatformAdapter *createWindowsAdapter(QObject *parent);
    PlatformAdapter *adapter = createWindowsAdapter(parent);
#elif defined(Q_OS_LINUX)
    extern PlatformAdapter *createLinuxAdapter(QObject *parent);
    PlatformAdapter *adapter = createLinuxAdapter(parent);
#else
    PlatformAdapter *adapter = nullptr;
#endif

    if (!adapter) {
        // 未支持平台：不崩溃，返回空指针，由 CoreRuntime 决定降级策略。
        // 但为了让程序仍能启动，这里应当提供一个最小实现。
        // 由于核心功能（桌宠 + 规则命令）必须可用（SRS 6.2），
        // 本项目的做法是在 PlatformAdapter_win/linux 中各自实现完整的
        // 通用回退逻辑，因此此处只记录严重问题。
        DC_FATAL(LogCat::Platform,
                 QStringLiteral("无法为当前系统创建平台适配器，"
                                "程序将无法执行文件与应用操作。"));
    }

    return adapter;
}

// ---------------------------------------------------------------------------
// 默认实现
// ---------------------------------------------------------------------------

bool PlatformAdapter::isPortableMode() const
{
    // 便携模式标记文件（SRS 8.4：旧系统可优先考虑便携包）
    const QString marker =
        QCoreApplication::applicationDirPath() + QStringLiteral("/portable.marker");
    return QFile::exists(marker);
}

QString PlatformAdapter::privilegeDescription() const
{
    if (isRunningAsAdmin()) {
        return QStringLiteral("管理员");
    }
    return QStringLiteral("普通用户");
}

QString PlatformAdapter::currentUserName() const
{
    // 优先使用环境变量（跨平台、不引入系统调用）
    const QString name = qEnvironmentVariable("USERNAME");
    if (!name.isEmpty())
        return name;

    const QString unixName = qEnvironmentVariable("USER");
    if (!unixName.isEmpty())
        return unixName;

    return QStringLiteral("unknown");
}

QStringList PlatformAdapter::systemApplicationHints() const
{
    // 返回「系统上大概有哪些常见应用」的提示，仅用于设置页的快捷填充。
    // 真正的应用路径必须由用户确认或从可信的系统应用列表导入（SRS 4.3）。
    QStringList hints;

#ifdef Q_OS_WIN
    const QString sysRoot = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    hints << sysRoot + QStringLiteral("/System32/notepad.exe")
          << sysRoot + QStringLiteral("/System32/mspaint.exe")
          << sysRoot + QStringLiteral("/System32/calc.exe")
          << sysRoot + QStringLiteral("/explorer.exe");
#elif defined(Q_OS_LINUX)
    // 只给出命令名，由用户确认具体路径
    hints << QStringLiteral("xdg-open")
          << QStringLiteral("firefox")
          << QStringLiteral("gedit");
#endif

    return hints;
}

// ---------------------------------------------------------------------------
// 按名称解析已安装程序
//
//  与 systemApplicationHints 的区别：这个是「真的去找出来并返回绝对路径」，
//  用于用户说"打开记事本"时的实现。核心要求是绝不搜索当前工作目录。
// ---------------------------------------------------------------------------
QString PlatformAdapter::resolveInstalledProgram(const QString &programName) const
{
    const QString name = programName.trimmed();
    if (name.isEmpty()) {
        return QString();
    }

    // 名字本身带路径分隔符的，不做搜索 —— 调用方应当走完整路径流程，
    // 让路径校验去负责安全性判断。这里返回空表示"我不管这种输入"。
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
        return QString();
    }

    // 候选文件名：Windows 需要补 .exe（以及 PATHEXT 里的其它扩展名）
    QStringList candidates;
    const QFileInfo rawName(name);
    if (!rawName.suffix().isEmpty()) {
        candidates << name;
    } else {
        candidates << name;
#ifdef Q_OS_WIN
        candidates << name + QStringLiteral(".exe")
                   << name + QStringLiteral(".com");
#endif
    }

    // 搜索目录：只取 PATH 中的绝对路径项 + 系统程序目录。
    // 关键点是跳过空项和相对项 —— 空的 PATH 项在 POSIX 语义下就是"当前目录"，
    // 这正是我们最需要避免的东西。
    QStringList searchDirs;

    const QString sysRoot = qEnvironmentVariable("SystemRoot");
#ifdef Q_OS_WIN
    if (!sysRoot.isEmpty()) {
        searchDirs << sysRoot + QStringLiteral("/System32")
                   << sysRoot + QStringLiteral("/SysWOW64")
                   << sysRoot;
    }
    // PATH：只保留绝对项
    const QStringList pathEntries =
        qEnvironmentVariable("PATH").split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &entry : pathEntries) {
        const QString trimmedEntry = entry.trimmed();
        if (trimmedEntry.isEmpty()) {
            continue;
        }
        // Windows 绝对路径形如 C:\... 或 \\server\share
        const QFileInfo entryInfo(trimmedEntry);
        if (entryInfo.isAbsolute()) {
            searchDirs << trimmedEntry;
        }
    }
#else
    // POSIX：常见应用目录优先，PATH 里只取以 '/' 开头的项
    searchDirs << QStringLiteral("/usr/local/bin")
               << QStringLiteral("/usr/bin")
               << QStringLiteral("/bin")
               << QStringLiteral("/usr/local/sbin")
               << QStringLiteral("/usr/sbin")
               << QStringLiteral("/sbin")
               << QStringLiteral("/opt/bin");
    const QStringList pathEntries =
        qEnvironmentVariable("PATH").split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &entry : pathEntries) {
        const QString trimmedEntry = entry.trimmed();
        if (trimmedEntry.startsWith(QLatin1Char('/'))) {
            searchDirs << trimmedEntry;
        }
    }
#endif

    // 去重，避免同一个目录被检查多次
    searchDirs.removeDuplicates();

    for (const QString &dir : searchDirs) {
        for (const QString &candidate : candidates) {
            const QString full = QDir(dir).filePath(candidate);
            const QFileInfo info(full);
            if (!info.exists() || !info.isFile()) {
                continue;
            }
#ifndef Q_OS_WIN
            if (!info.isExecutable()) {
                continue;
            }
#endif
            // 解析符号链接后再返回，让上层的路径校验看到真实目标。
            // 否则一个指向 /tmp 的软链接可能骗过白名单检查。
            const QString canonical = info.canonicalFilePath();
            return canonical.isEmpty() ? full : canonical;
        }
    }

    return QString();
}

} // namespace dc
