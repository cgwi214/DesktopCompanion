// ============================================================================
//  PlatformAdapter_linux.cpp — Linux 平台适配实现
//
//  需求依据：SRS V1.1 4.3、4.4、6.1、6.2、7.3、8.4
//
//  支持范围（SRS 6.1 兼容策略）：
//    CentOS 6 / RHEL 7 等老发行版与新发行版共用同一份源码。
//    老系统上缺失的能力（如音频后端、D-Bus 密钥环）由运行时能力探测处理，
//    不在编译期排除，以免出现「在同一发行版的不同版本上行为不一致」。
//
//  安全要点：
//    启动外部程序时同样使用「参数数组 + 不经过 shell」，与 Windows 侧一致。
// ============================================================================
#include "platform/PlatformAdapter.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>

// geteuid() 在 <unistd.h> 里。用 geteuid 而不是 getuid：
// 前者看"有效用户 id"，能正确反映 setuid 程序的实际权限。
//
// 注意这个 include 必须加平台判断：本文件在 Windows 上也会被编译
// （整个实现体在下方用 #ifdef Q_OS_LINUX 包住），而 Windows 的 MinGW
// 并没有 geteuid。不加判断会在 Windows 上直接编译失败。
#ifdef Q_OS_LINUX
#  include <unistd.h>
#  include <sys/types.h>
#endif

#ifdef Q_OS_LINUX

namespace dc {

// ===========================================================================
//  Linux 适配器
// ===========================================================================
class LinuxAdapter : public PlatformAdapter
{
public:
    explicit LinuxAdapter(QObject *parent = nullptr)
        : PlatformAdapter(parent)
    {
    }

    QString platformId() const override { return QStringLiteral("linux"); }

    // -----------------------------------------------------------------------
    // 启动进程
    // -----------------------------------------------------------------------
    OpenTargetResult launchProcess(const QString &executablePath,
                                  const QStringList &arguments,
                                  const QString &workingDir) override
    {
        OpenTargetResult result;

        const QFileInfo info(executablePath);
        if (!info.exists() || !info.isFile()) {
            result.errorCode = erc::AppPathMissing;
            result.message = erc::describe(erc::AppPathMissing);
            return result;
        }
        if (!info.isAbsolute()) {
            result.errorCode = erc::AppPathMissing;
            result.message = QStringLiteral("应用路径必须是绝对路径。");
            return result;
        }
        if (!info.isExecutable()) {
            result.errorCode = erc::AppStartFailed;
            result.message = QStringLiteral("该文件没有可执行权限。");
            return result;
        }

        if (arguments.size() > 32) {
            result.errorCode = erc::AppArgsRejected;
            result.message = QStringLiteral("启动参数数量过多，已拒绝执行。");
            return result;
        }
        for (const QString &arg : arguments) {
            if (arg.length() > 512 || arg.contains(QChar(QChar::Null))) {
                result.errorCode = erc::AppArgsRejected;
                result.message = QStringLiteral("启动参数不合法，已拒绝执行。");
                return result;
            }
        }

        // 与 Windows 侧同样的解释器拦截（SRS 4.6 / 7.3）
        const QString baseName = info.fileName().toLower();
        static const QStringList interpreters{
            QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
            QStringLiteral("dash"), QStringLiteral("python"), QStringLiteral("python3"),
            QStringLiteral("perl"), QStringLiteral("ruby"), QStringLiteral("env")
        };
        if (interpreters.contains(baseName)) {
            result.errorCode = erc::AppArgsRejected;
            result.message = QStringLiteral(
                "出于安全考虑，本程序不允许把命令解释器（%1）作为应用启动。"
                "如确需运行脚本，请在设置中启用「脚本执行」并逐次授权。")
                .arg(baseName);
            DC_WARN(LogCat::System,
                    QStringLiteral("已阻止启动命令解释器：%1").arg(PathGuard::redact(executablePath)));
            return result;
        }

        auto *process = new QProcess(this);
        if (!workingDir.isEmpty() && QFileInfo(workingDir).isDir())
            process->setWorkingDirectory(workingDir);
        process->setProcessChannelMode(QProcess::ForwardedChannels);

        process->start(executablePath, arguments, QIODevice::NotOpen);

        if (!process->waitForStarted(3000)) {
            const QString err = process->errorString();
            process->deleteLater();
            result.errorCode = erc::AppStartFailed;
            result.message = erc::describe(erc::AppStartFailed);
            result.message += QStringLiteral("（%1）").arg(err);
            DC_ERROR(LogCat::System,
                     QStringLiteral("启动应用失败：%1 - %2")
                         .arg(PathGuard::redact(executablePath), err));
            return result;
        }

        result.ok = true;
        result.processId = process->processId();

        DC_INFO(LogCat::System,
                QStringLiteral("已启动应用：%1 (pid=%2)")
                    .arg(PathGuard::redact(executablePath))
                    .arg(result.processId));

        process->disconnect();
        process->deleteLater();
        return result;
    }

    // -----------------------------------------------------------------------
    // 用系统默认程序打开
    //
    //  Linux 上的等价物是 xdg-open。必须传「路径数组」形式，
    //  且路径不得以 '-' 开头（否则会被当作选项解析 —— 这是经典漏洞）。
    // -----------------------------------------------------------------------
    OpenTargetResult openWithDefaultApplication(const QString &absolutePath) override
    {
        OpenTargetResult result;

        const QFileInfo info(absolutePath);
        if (!info.exists()) {
            result.errorCode = erc::PathNotFound;
            result.message = erc::describe(erc::PathNotFound);
            return result;
        }

        const QString target = info.absoluteFilePath();

        // 参数注入防护：以 '-' 开头的路径会被 xdg-open 当作命令行选项
        if (QFileInfo(target).fileName().startsWith(QLatin1Char('-'))) {
            result.errorCode = erc::PathInvalidCharacters;
            result.message = QStringLiteral("文件名以连字符开头，出于安全考虑无法直接打开。");
            return result;
        }

        const QString opener = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
        if (opener.isEmpty()) {
            result.errorCode = erc::PlatformCapabilityMissing;
            result.message = QStringLiteral(
                "本系统未安装 xdg-open，无法用默认程序打开文件。"
                "可安装 xdg-utils 后重试。");
            return result;
        }

        auto *process = new QProcess(this);
        // 参数数组形式：target 作为独立参数传入，不参与 shell 解析
        process->start(opener, { target }, QIODevice::NotOpen);

        if (!process->waitForStarted(3000)) {
            const QString err = process->errorString();
            process->deleteLater();
            result.errorCode = erc::PathNotAccessible;
            result.message = QStringLiteral("无法启动默认程序：%1").arg(err);
            return result;
        }

        process->disconnect();
        process->deleteLater();

        result.ok = true;
        DC_INFO(LogCat::System,
                QStringLiteral("已用默认程序打开：%1").arg(PathGuard::redact(target)));
        return result;
    }

    // -----------------------------------------------------------------------
    // 在文件管理器中定位
    // -----------------------------------------------------------------------
    OpenTargetResult revealInFileManager(const QString &absolutePath) override
    {
        OpenTargetResult result;

        const QFileInfo info(absolutePath);
        if (!info.exists()) {
            result.errorCode = erc::PathNotFound;
            result.message = erc::describe(erc::PathNotFound);
            return result;
        }

        // 优先使用 dbus 让已运行的文件管理器选中文件；
        // 老系统上 dbus 不可用则退化为打开父目录。
        const QString dbusSend = QStandardPaths::findExecutable(QStringLiteral("dbus-send"));

        if (!dbusSend.isEmpty()) {
            QStringList args{
                QStringLiteral("--session"),
                QStringLiteral("--dest=org.freedesktop.FileManager1"),
                QStringLiteral("--type=method_call"),
                QStringLiteral("/org/freedesktop/FileManager1"),
                QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                QStringLiteral("array:string:file://") + info.absoluteFilePath(),
                QStringLiteral("string:")
            };

            auto *process = new QProcess(this);
            process->start(dbusSend, args, QIODevice::NotOpen);
            if (!process->waitForStarted(2000)) {
                process->deleteLater();
            } else {
                process->disconnect();
                process->deleteLater();
                result.ok = true;
                return result;
            }
        }

        // 回退：打开父目录
        return openWithDefaultApplication(info.absolutePath());
    }

    // -----------------------------------------------------------------------
    // 路径
    // -----------------------------------------------------------------------
    QString homeDirectory() const override
    {
        const QString p = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
        return p.isEmpty() ? QDir::homePath() : p;
    }

    QString desktopDirectory() const override
    {
        // XDG 用户目录可能未配置，回退到 ~/Desktop
        const QString p = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        return p.isEmpty() ? homeDirectory() + QStringLiteral("/Desktop") : p;
    }

    QString documentsDirectory() const override
    {
        const QString p = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        return p.isEmpty() ? homeDirectory() + QStringLiteral("/Documents") : p;
    }

    QString downloadsDirectory() const override
    {
        const QString p = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        return p.isEmpty() ? homeDirectory() + QStringLiteral("/Downloads") : p;
    }

    // -----------------------------------------------------------------------
    // 系统信息
    // -----------------------------------------------------------------------
    QString osName() const override
    {
        return QStringLiteral("Linux");
    }

    QString osVersionString() const override
    {
        return QSysInfo::kernelVersion();
    }

    // -----------------------------------------------------------------------
    // 权限检查
    //
    //  SRS 4.4：默认以普通用户权限运行，不静默提权。
    //  本函数只如实报告，绝不尝试提权。
    // -----------------------------------------------------------------------
    bool isRunningAsAdmin() const override
    {
        return ::geteuid() == 0;
    }

    // -----------------------------------------------------------------------
    // 窗口能力
    // -----------------------------------------------------------------------
    bool supportsTranslucentWindow() const override
    {
        // Wayland 下由合成器决定，Qt 通常可用；
        // X11 下需要合成管理器（compositor）在运行，否则半透明无效。
        const bool wayland = !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty();
        if (wayland)
            return true;

        // 简单探测：_NET_WM_CM_S0 所有者存在则说明有合成器。
        // 这里不做 X11 调用以避免引入依赖，改由 Qt 侧尝试失败后降级。
        return true;
    }

    bool supportsAlwaysOnTop() const override
    {
        return true;
    }

    bool applyNoFocusPolicy(void *nativeWindowHandle) override
    {
        // Linux 侧的「不抢焦点」由 Qt 窗口标志处理（Qt::WindowDoesNotAcceptFocus），
        // 在 PetWindow 中统一设置，因此这里不需要额外原生调用。
        Q_UNUSED(nativeWindowHandle)
        return true;
    }

    bool beep() override
    {
        // 写终端响铃字符，不依赖音频库
        QFile tty(QStringLiteral("/dev/tty"));
        if (tty.open(QIODevice::WriteOnly)) {
            tty.write("\a");
            tty.close();
            return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// 工厂导出
// ---------------------------------------------------------------------------
PlatformAdapter *createLinuxAdapter(QObject *parent)
{
    return new LinuxAdapter(parent);
}

} // namespace dc

#else // !Q_OS_LINUX

// ---------------------------------------------------------------------------
// 非 Linux 平台：不提供 Linux 适配器实现。
//
// 本文件在 Windows 构建中也会被纳入编译（sources.cmake 用 GLOB 收集），
// 因此这里显式给出一个空的编译单元，保证"文件存在但无符号"，
// 而工厂函数 createLinuxAdapter() 由调用方按平台条件引用。
// ---------------------------------------------------------------------------

#endif // Q_OS_LINUX
