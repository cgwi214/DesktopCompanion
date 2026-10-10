// ============================================================================
//  PlatformAdapter_win.cpp — Windows 平台适配实现
//
//  需求依据：SRS V1.1 4.3、4.4、6.1、6.2、7.3、8.4
//
//  支持范围（SRS 6.1 兼容策略）：
//    Windows XP / 7 / 新版 Windows —— 均使用同一份源码，
//    差异通过「运行时能力探测 + 动态加载」而非编译期排除来处理。
//
//  为什么用动态加载（GetProcAddress）而不是直接链接：
//    直接调用只在 Win8+ 存在的 API 会导致程序在 Win7 上启动即失败
//    （导入表缺失）。SRS 6.2 明确要求「避免依赖仅在新系统提供的 API；
//    必要时提供条件编译或替代实现」——动态加载就是这里的替代实现。
// ============================================================================
#include "platform/PlatformAdapter.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace dc {

// ===========================================================================
//  Windows 适配器
// ===========================================================================
class WindowsAdapter : public PlatformAdapter
{
public:
    explicit WindowsAdapter(QObject *parent = nullptr)
        : PlatformAdapter(parent)
    {
    }

    QString platformId() const override { return QStringLiteral("windows"); }

    // -----------------------------------------------------------------------
    // 启动进程
    //
    //  安全要点（SRS 7.3：避免 shell 字符串拼接和命令注入）：
    //    使用 QProcess::start(program, arguments) 的重载 ——
    //    该重载在 Windows 上使用 CreateProcess 的参数数组形式，
    //    不经过 cmd.exe，因此参数中的 & | > < ^ 等字符不具有特殊含义。
    //    绝不允许把参数拼成单一字符串再交给 shell。
    // -----------------------------------------------------------------------
    OpenTargetResult launchProcess(const QString &executablePath,
                                  const QStringList &arguments,
                                  const QString &workingDir) override
    {
        OpenTargetResult result;

        // 前置校验：必须是绝对路径且真实存在
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

        // 参数数量与长度限制：防止把超长/大量参数当作攻击载荷（SRS 7.3）
        if (arguments.size() > 32) {
            result.errorCode = erc::AppArgsRejected;
            result.message = QStringLiteral("启动参数数量过多（%1），已拒绝执行。")
                                 .arg(arguments.size());
            return result;
        }
        for (const QString &arg : arguments) {
            if (arg.length() > 512) {
                result.errorCode = erc::AppArgsRejected;
                result.message = QStringLiteral("启动参数过长，已拒绝执行。");
                return result;
            }
            // 含 NUL 的参数必然异常
            if (arg.contains(QChar(QChar::Null))) {
                result.errorCode = erc::AppArgsRejected;
                result.message = QStringLiteral("启动参数包含非法字符，已拒绝执行。");
                return result;
            }
        }

        // 阻止直接启动命令解释器：即使用户配置了 cmd/powershell，
        // 也必须在设置中显式确认；这里返回明确错误而不是静默放行。
        const QString baseName = info.completeBaseName().toLower();
        static const QStringList interpreters{
            QStringLiteral("cmd"), QStringLiteral("powershell"),
            QStringLiteral("pwsh"), QStringLiteral("wscript"),
            QStringLiteral("cscript"), QStringLiteral("mshta"),
            QStringLiteral("rundll32"), QStringLiteral("regsvr32"),
            QStringLiteral("certutil"), QStringLiteral("bitsadmin"),
            QStringLiteral("msiexec"), QStringLiteral("wmic")
        };
        if (interpreters.contains(baseName)) {
            result.errorCode = erc::AppArgsRejected;
            result.message = QStringLiteral(
                "出于安全考虑，本程序不允许把系统命令解释器（%1）作为应用启动。"
                "如确需运行脚本，请在设置中启用「脚本执行」并逐次授权。")
                .arg(baseName);
            DC_WARN(LogCat::System,
                    QStringLiteral("已阻止启动命令解释器：%1").arg(PathGuard::redact(executablePath)));
            return result;
        }

        auto *process = new QProcess(this);
        if (!workingDir.isEmpty() && QFileInfo(workingDir).isDir())
            process->setWorkingDirectory(workingDir);

        // 断开进程对象与其父进程的生命周期耦合：应用启动后应独立运行
        process->setProcessChannelMode(QProcess::ForwardedChannels);

        // 关键：参数数组重载，不经过 shell
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

        // 进程对象由 Qt 管理，分离后自动回收
        process->disconnect();
        process->deleteLater();

        return result;
    }

    // -----------------------------------------------------------------------
    // 用系统默认程序打开
    //
    //  这里使用 ShellExecuteW 而不是 QDesktopServices::openUrl，
    //  原因是需要拿到 ShellExecute 的错误码以给出「为什么打不开」的
    //  可理解提示（SRS 4.3：「显示可理解的错误信息」）。
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

        // ShellExecuteW 要求反斜杠形式的路径
        QString nativePath = QDir::toNativeSeparators(info.absoluteFilePath());

        const HINSTANCE hResult = ::ShellExecuteW(
            nullptr,
            L"open",
            reinterpret_cast<const wchar_t *>(nativePath.utf16()),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);

        // ShellExecute 的返回值 > 32 才表示成功
        const INT_PTR code = reinterpret_cast<INT_PTR>(hResult);
        if (code <= 32) {
            result.errorCode = erc::PathNotAccessible;

            QString reason;
            switch (code) {
            case 0:  reason = QStringLiteral("系统资源不足。"); break;
            case 2:  reason = QStringLiteral("找不到指定的文件。"); break;
            case 3:  reason = QStringLiteral("找不到指定的路径。"); break;
            case 5:  reason = QStringLiteral("访问被拒绝，权限不足。"); break;
            case 8:  reason = QStringLiteral("内存不足。"); break;
            case 26: reason = QStringLiteral("文件共享冲突。"); break;
            case 27: reason = QStringLiteral("文件关联不完整或无效。"); break;
            case 28: reason = QStringLiteral("DDE 操作超时。"); break;
            case 29: reason = QStringLiteral("DDE 操作失败。"); break;
            case 30: reason = QStringLiteral("DDE 操作被占用。"); break;
            case 31: reason = QStringLiteral("没有可用于打开该文件的关联程序。"); break;
            case 32: reason = QStringLiteral("无法加载所需的动态链接库。"); break;
            default: reason = QStringLiteral("系统返回错误码 %1。").arg(code); break;
            }

            result.message = QStringLiteral("无法打开该文件：%1").arg(reason);

            DC_WARN(LogCat::System,
                    QStringLiteral("打开文件失败：%1（ShellExecute 返回 %2）")
                        .arg(PathGuard::redact(absolutePath))
                        .arg(code));
            return result;
        }

        result.ok = true;
        DC_INFO(LogCat::System,
                QStringLiteral("已用默认程序打开：%1").arg(PathGuard::redact(absolutePath)));
        return result;
    }

    // -----------------------------------------------------------------------
    // 在资源管理器中定位文件
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

        // explorer.exe /select,"path" 的参数形式。
        // 这里用 QProcess 的参数数组形式启动，避免引号拼接问题。
        QStringList args;
        args << QStringLiteral("/select,") + QDir::toNativeSeparators(info.absoluteFilePath());

        auto *process = new QProcess(this);
        process->start(QStringLiteral("explorer.exe"), args, QIODevice::NotOpen);

        // explorer.exe 即使成功也可能不返回成功码，因此不严格等待
        process->waitForStarted(2000);
        process->disconnect();
        process->deleteLater();

        result.ok = true;
        return result;
    }

    // -----------------------------------------------------------------------
    // 路径
    // -----------------------------------------------------------------------
    QString homeDirectory() const override
    {
        return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }

    QString desktopDirectory() const override
    {
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
        // Windows XP 没有 Downloads 的已知文件夹 GUID，
        // QStandardPaths 会回退到 Documents 或空，这里手动兜底。
        QString p = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (p.isEmpty())
            p = homeDirectory() + QStringLiteral("/Downloads");
        return p;
    }

    // -----------------------------------------------------------------------
    // 系统信息
    // -----------------------------------------------------------------------
    QString osName() const override
    {
        return QStringLiteral("Windows");
    }

    QString osVersionString() const override
    {
        // 使用 RtlGetVersion 而非 GetVersionEx：
        // 后者在没有清单文件时会对 Win8.1+ 返回 6.2（兼容性谎言）
        typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

        const HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            const auto fn = reinterpret_cast<RtlGetVersionPtr>(
                ::GetProcAddress(hNtdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn(&info) == 0) {
                    return QStringLiteral("%1.%2.%3")
                        .arg(info.dwMajorVersion)
                        .arg(info.dwMinorVersion)
                        .arg(info.dwBuildNumber);
                }
            }
        }
        return QStringLiteral("unknown");
    }

    // -----------------------------------------------------------------------
    // 权限检查
    //
    //  SRS 4.4：软件默认以普通用户权限运行，不得暗中取得管理员权限。
    //  本函数只用于「如实告知用户当前权限状态」，不做任何提权动作。
    // -----------------------------------------------------------------------
    bool isRunningAsAdmin() const override
    {
        BOOL isMember = FALSE;
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID adminGroup = nullptr;

        if (!::AllocateAndInitializeSid(&authority, 2,
                                        SECURITY_BUILTIN_DOMAIN_RID,
                                        DOMAIN_ALIAS_RID_ADMINS,
                                        0, 0, 0, 0, 0, 0,
                                        &adminGroup)) {
            return false;
        }

        const BOOL ok = ::CheckTokenMembership(nullptr, adminGroup, &isMember);
        ::FreeSid(adminGroup);

        return ok && isMember;
    }

    // -----------------------------------------------------------------------
    // 窗口能力
    // -----------------------------------------------------------------------
    bool supportsTranslucentWindow() const override
    {
        // Windows Vista (6.0) 引入 DWM 桌面合成。
        // 这里探测 DWM 是否实际在运行 —— 因为 Win7 Basic 主题下
        // DWM 可能被关闭，此时半透明会退化为黑色背景。
        typedef HRESULT(WINAPI * DwmIsCompositionEnabledPtr)(BOOL *);

        const HMODULE hDwm = ::LoadLibraryW(L"dwmapi.dll");
        if (!hDwm)
            return false;   // XP 没有 dwmapi.dll

        const auto fn = reinterpret_cast<DwmIsCompositionEnabledPtr>(
            ::GetProcAddress(hDwm, "DwmIsCompositionEnabled"));

        BOOL enabled = FALSE;
        const bool result = fn && SUCCEEDED(fn(&enabled)) && enabled;

        ::FreeLibrary(hDwm);
        return result;
    }

    bool supportsAlwaysOnTop() const override
    {
        return true;   // Windows 一直支持 SetWindowPos 的 HWND_TOPMOST
    }

    // -----------------------------------------------------------------------
    // 不抢占焦点
    //
    //  SRS 4.1：「桌宠窗口应尽量避免抢占焦点。」
    //  通过扩展窗口样式 WS_EX_NOACTIVATE 实现：点击桌宠不会夺走
    //  当前活动窗口的焦点，用户可以在不打字中断的情况下拖动桌宠。
    // -----------------------------------------------------------------------
    bool applyNoFocusPolicy(void *nativeWindowHandle) override
    {
        if (!nativeWindowHandle)
            return false;

        const HWND hwnd = static_cast<HWND>(nativeWindowHandle);

        LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (style == 0)
            return false;

        style |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;

        // WS_EX_TOOLWINDOW 让桌宠不出现在 Alt+Tab 列表里 —— 这正是桌宠
        // 应有的行为，避免干扰用户的窗口切换。
        if (::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style) == 0)
            return false;

        return true;
    }

    // -----------------------------------------------------------------------
    // 声音提示
    // -----------------------------------------------------------------------
    bool beep() override
    {
        // 使用系统提示音，避免自带音频资源的版权与体积问题
        return ::MessageBeep(MB_OK) != FALSE;
    }
};

// ---------------------------------------------------------------------------
// 工厂导出
// ---------------------------------------------------------------------------
PlatformAdapter *createWindowsAdapter(QObject *parent)
{
    return new WindowsAdapter(parent);
}

} // namespace dc
