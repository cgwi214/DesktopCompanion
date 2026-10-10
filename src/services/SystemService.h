#pragma once

// ============================================================================
//  SystemService —— 所有系统操作的唯一出口（SRS 3.2）
//
//  需求原文（SRS 3.2 模块依赖规则）：
//    "UI 层不得直接执行系统命令或访问网络；所有系统操作统一通过 SystemService。"
//
//  这条规则是硬约束，不是建议。实现方式：
//    - SystemService 是本工程中唯一调用 PlatformAdapter 的模块；
//    - 每个公开方法都强制走 PathGuard::check()，不做例外；
//    - 本类不提供"绕过检查"的重载 —— 想绕过得改这个文件，改不了调用点。
//
//  职责：
//    1. 启动应用程序 / 打开文件与网址（SRS 4.5）
//    2. 在文件管理器中定位文件
//    3. 打开系统设置页（辅助用户手动完成高风险操作）
//    4. 系统信息与能力查询（转发给 SystemInfo，避免 UI 直接依赖平台层）
//    5. 剪贴板
//
//  不负责：
//    - 权限判定（那是 PermissionManager 的职责，SystemService 只管"执行"）
//    - 路径合法性之外的安全策略
//
//  重要安全约定：
//    - 拒绝一切解释器型程序（cmd / powershell / bash / python ...）作为启动目标，
//      因为它们能把"启动一个程序"变成"执行任意代码"，绕过权限分级。
//    - 使用参数数组启动（QProcess::start(program, args)），绝不拼接命令行字符串，
//      从根上消除 shell 注入面。
//    - 不用默认程序打开脚本类文件（.bat/.ps1/.vbs/.lnk ...），否则等于变相执行脚本。
// ============================================================================

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/ErrorCode.h"
#include "platform/PathGuard.h"
#include "platform/SystemInfo.h"

namespace dc {

// ---------------------------------------------------------------------------
// 执行结果
//
//  errorCode 沿用 core/ErrorCode.h 的 erc:: 体系（QString 形式），
//  好处是这条结果可以直接透传到 UI，又能进日志与审计而无需再转换。
// ---------------------------------------------------------------------------
struct SystemActionResult {
    bool ok = false;
    QString errorCode;      // erc:: 中的常量，例如 erc::PathNotAllowed
    QString message;        // 面向用户的中文说明（可直接显示）
    QString detail;         // 面向日志的技术细节（调用方负责脱敏后才落盘）

    static SystemActionResult success()
    {
        SystemActionResult r;
        r.ok = true;
        r.errorCode = QString::fromLatin1(erc::Ok);
        return r;
    }

    static SystemActionResult failure(const QString &code,
                                      const QString &message,
                                      const QString &detail = QString())
    {
        SystemActionResult r;
        r.ok = false;
        r.errorCode = code;
        r.message = message;
        r.detail = detail;
        return r;
    }
};

// ---------------------------------------------------------------------------
// 启动参数
// ---------------------------------------------------------------------------
struct LaunchOptions {
    // 工作目录（空 = 交给系统默认行为）
    QString workingDirectory;

    // 附加参数（按参数数组传入，不经过 shell，因此无需转义）
    QStringList arguments;

    // 是否允许启动已知的交互式终端/解释器。
    // 默认 false，且强烈不建议打开：它会让"启动程序"等价于"执行任意命令"。
    bool allowInterpreter = false;

    // 是否允许启动位于系统目录下的可执行文件。
    // 默认 false —— 打开系统目录里的程序通常意味着绕过应用分发渠道，
    // 需要用户显式授权。
    bool allowSystemExecutable = false;
};

// ---------------------------------------------------------------------------
// 打开文件/网址的参数
// ---------------------------------------------------------------------------
struct OpenOptions {
    // 路径是否必须落在白名单根目录内。
    // 对用户明确选择的本机文件可放宽为 false，但仍会经过 PathGuard 的
    // 非法字符、穿越、保留名、UNC 检查。
    bool requireWhitelist = true;

    // 允许的扩展名白名单（空 = 不按扩展名限制）
    QStringList allowedExtensions;
};

// ============================================================================
// SystemService
// ============================================================================
class SystemService : public QObject
{
    Q_OBJECT

public:
    explicit SystemService(QObject *parent = nullptr);
    ~SystemService() override;

    // 注入路径策略（由 CoreRuntime 在启动时装配）
    void setPathPolicy(const PathPolicy &policy);
    PathPolicy pathPolicy() const;

    // ---- 系统信息（转发，避免 UI 直接依赖平台层）----

    SystemSnapshot snapshot() const;
    CapabilityReport capabilities() const;

    // 判断某项能力是否可用。用枚举而不是字符串，避免拼写错误导致误判。
    enum class Cap {
        TranslucentWindow,
        AlwaysOnTop,
        TrayIcon,
        GlobalHotkey,
        Autostart,
        NetworkTls12,
        SystemCaStore,
        AudioInput,
        AudioOutput,
        SpeechRecognition,
        SpeechSynthesis,
        SecureStorage,
        DesktopNotifications,
        ImageSequenceLoad
    };
    bool isCapAvailable(Cap cap) const;
    QString capReason(Cap cap) const;   // 不可用原因（可直接展示给用户）

    QString buildCompatibilityReport() const;
    bool isRunningAsAdmin() const;
    QString privilegeDescription() const;

    // ---- 4.5.1 启动应用程序 ----

    // path 必须是文件路径；同时过 PathGuard 与解释器/系统目录检查。
    SystemActionResult launchApplication(const QString &path, const LaunchOptions &options);

    // 按程序名启动已安装程序（例如 "notepad"）。
    //
    // 实现说明：不做 PATH 搜索后直接执行。理由：在 Windows 上当前目录会优先于
    // PATH 被搜索，攻击者只要在工作目录里放一个同名 exe 就能劫持该调用。
    // 这里通过在受信任的系统程序目录中查找来规避。
    SystemActionResult launchInstalledProgram(const QString &programName,
                                              const LaunchOptions &options = LaunchOptions());

    // ---- 4.5.2 用默认程序打开文件 ----
    SystemActionResult openPath(const QString &path, const OpenOptions &options = OpenOptions());

    // ---- 4.5.3 在文件管理器中定位 ----
    SystemActionResult revealInFileManager(const QString &path);

    // ---- 打开网址（默认浏览器）----
    // 只允许 http/https，其它协议一律拒绝（避免 file://、自定义协议成为旁路）。
    SystemActionResult openUrl(const QString &url);

    // ---- 打开系统设置页（辅助用户手动完成高风险操作）----
    enum class SystemSettingsPage {
        EnvironmentVariables,
        StartupApps,
        Microphone,
        Sound,
        Network
    };
    SystemActionResult openSystemSettings(SystemSettingsPage page);

    // ---- 剪贴板 ----
    QString clipboardText() const;
    SystemActionResult setClipboardText(const QString &text);

    // ---- 提示音 ----
    SystemActionResult beep();

private:
    // 统一的前置校验：路径合法性 + 解释器黑名单 + 系统目录限制
    SystemActionResult validateLaunchTarget(const QString &path,
                                            const LaunchOptions &options,
                                            QString *resolved) const;

    // 解释器/终端黑名单 —— 这类程序不进入权限分级体系，直接拒绝
    static bool isInterpreterProgram(const QString &baseNameLower);

    // 脚本类扩展名 —— 不允许用"默认程序"打开
    static bool isScriptExtension(const QString &suffixLower);

    static bool extensionAllowed(const QString &path, const QStringList &allowed);

    PathPolicy m_policy;
};

} // namespace dc
