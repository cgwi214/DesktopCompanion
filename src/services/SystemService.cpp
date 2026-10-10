#include "SystemService.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PlatformAdapter.h"

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QUrl>

namespace dc {

// 把 erc:: 的 const char* 常量统一转成 QString，避免在调用点反复写 fromLatin1
static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ============================================================================
// 构造
// ============================================================================
SystemService::SystemService(QObject *parent)
    : QObject(parent)
    , m_policy(PathPolicy{})
{
    // 默认策略：以当前用户目录为允许根，而不是放开整个文件系统。
    // 具体值由 CoreRuntime 在装配时用配置覆盖。
    m_policy.allowedRoots = PathGuard::defaultAllowedRoots();
    m_policy.requireWhitelist = true;
    m_policy.allowSensitiveLocations = true;
    m_policy.requireExists = true;
}

SystemService::~SystemService() = default;

void SystemService::setPathPolicy(const PathPolicy &policy)
{
    m_policy = policy;
}

PathPolicy SystemService::pathPolicy() const
{
    return m_policy;
}

// ============================================================================
// 系统信息转发
// ============================================================================
SystemSnapshot SystemService::snapshot() const
{
    return SystemInfo::current();
}

CapabilityReport SystemService::capabilities() const
{
    return SystemInfo::current().capabilities;
}

bool SystemService::isCapAvailable(Cap cap) const
{
    const CapabilityReport report = SystemInfo::current().capabilities;
    switch (cap) {
    case Cap::TranslucentWindow:     return report.translucentWindow.available;
    case Cap::AlwaysOnTop:           return report.alwaysOnTop.available;
    case Cap::TrayIcon:              return report.trayIcon.available;
    case Cap::GlobalHotkey:          return report.globalHotkey.available;
    case Cap::Autostart:             return report.autostart.available;
    case Cap::NetworkTls12:          return report.networkTls12.available;
    case Cap::SystemCaStore:         return report.systemCaStore.available;
    case Cap::AudioInput:            return report.audioInput.available;
    case Cap::AudioOutput:           return report.audioOutput.available;
    case Cap::SpeechRecognition:     return report.speechRecognition.available;
    case Cap::SpeechSynthesis:       return report.speechSynthesis.available;
    case Cap::SecureStorage:         return report.secureStorage.available;
    case Cap::DesktopNotifications:  return report.desktopNotifications.available;
    case Cap::ImageSequenceLoad:     return report.imageSequenceLoad.available;
    }
    // 未知枚举值 -> 视为不可用（失败安全：宁可少给功能，不给未验证功能）
    return false;
}

QString SystemService::capReason(Cap cap) const
{
    const CapabilityReport report = SystemInfo::current().capabilities;
    switch (cap) {
    case Cap::TranslucentWindow:     return report.translucentWindow.reason;
    case Cap::AlwaysOnTop:           return report.alwaysOnTop.reason;
    case Cap::TrayIcon:              return report.trayIcon.reason;
    case Cap::GlobalHotkey:          return report.globalHotkey.reason;
    case Cap::Autostart:             return report.autostart.reason;
    case Cap::NetworkTls12:          return report.networkTls12.reason;
    case Cap::SystemCaStore:         return report.systemCaStore.reason;
    case Cap::AudioInput:            return report.audioInput.reason;
    case Cap::AudioOutput:           return report.audioOutput.reason;
    case Cap::SpeechRecognition:     return report.speechRecognition.reason;
    case Cap::SpeechSynthesis:       return report.speechSynthesis.reason;
    case Cap::SecureStorage:         return report.secureStorage.reason;
    case Cap::DesktopNotifications:  return report.desktopNotifications.reason;
    case Cap::ImageSequenceLoad:     return report.imageSequenceLoad.reason;
    }
    return tr("该能力当前不可用。");
}

QString SystemService::buildCompatibilityReport() const
{
    return SystemInfo::buildCompatibilityReport(SystemInfo::current());
}

bool SystemService::isRunningAsAdmin() const
{
    PlatformAdapter *adapter = PlatformAdapter::instance();
    return adapter ? adapter->isRunningAsAdmin() : false;
}

QString SystemService::privilegeDescription() const
{
    PlatformAdapter *adapter = PlatformAdapter::instance();
    return adapter ? adapter->privilegeDescription() : QString();
}

// ============================================================================
// 解释器黑名单
//
//  为什么单独拦这一类：
//    权限分级是按"规则 + 目标指纹"授权的（SRS 4.4）。如果允许启动 cmd.exe，
//    那么"打开应用程序"这条低风险规则实际就变成了"执行任意命令"，
//    授权绑定会被完全架空。所以这类程序不进入分级体系，直接拒绝。
//
//  注意：这里拦的是「作为启动目标」。用户在命令里输入的参数仍会经过
//  PermissionManager 的参数校验，两者互不替代。
// ============================================================================
bool SystemService::isInterpreterProgram(const QString &baseNameLower)
{
    static const QStringList kBlocked = {
        // Windows 自带解释器与"能被用来下载并执行"的工具
        QStringLiteral("cmd"), QStringLiteral("cmd.exe"),
        QStringLiteral("powershell"), QStringLiteral("powershell.exe"),
        QStringLiteral("pwsh"), QStringLiteral("pwsh.exe"),
        QStringLiteral("wscript"), QStringLiteral("wscript.exe"),
        QStringLiteral("cscript"), QStringLiteral("cscript.exe"),
        QStringLiteral("mshta"), QStringLiteral("mshta.exe"),
        QStringLiteral("rundll32"), QStringLiteral("rundll32.exe"),
        QStringLiteral("regsvr32"), QStringLiteral("regsvr32.exe"),
        QStringLiteral("reg"), QStringLiteral("reg.exe"),
        QStringLiteral("conhost"), QStringLiteral("conhost.exe"),
        QStringLiteral("wt"), QStringLiteral("wt.exe"),
        QStringLiteral("cmstp"), QStringLiteral("cmstp.exe"),
        QStringLiteral("installutil"), QStringLiteral("installutil.exe"),
        QStringLiteral("certutil"), QStringLiteral("certutil.exe"),
        QStringLiteral("bitsadmin"), QStringLiteral("bitsadmin.exe"),
        // POSIX shell
        QStringLiteral("sh"), QStringLiteral("bash"), QStringLiteral("zsh"),
        QStringLiteral("dash"), QStringLiteral("ksh"), QStringLiteral("csh"),
        QStringLiteral("tcsh"), QStringLiteral("fish"), QStringLiteral("ash"),
        QStringLiteral("env"), QStringLiteral("xargs"), QStringLiteral("eval"),
        QStringLiteral("sudo"), QStringLiteral("su"), QStringLiteral("doas"),
        QStringLiteral("pkexec"), QStringLiteral("gksudo"),
        // 脚本语言运行时（都能一行起个反向 shell）
        QStringLiteral("python"), QStringLiteral("python.exe"),
        QStringLiteral("python3"), QStringLiteral("pythonw.exe"),
        QStringLiteral("perl"), QStringLiteral("perl.exe"),
        QStringLiteral("ruby"), QStringLiteral("ruby.exe"),
        QStringLiteral("node"), QStringLiteral("node.exe"),
        QStringLiteral("php"), QStringLiteral("php.exe"),
        QStringLiteral("java"), QStringLiteral("java.exe"),
        QStringLiteral("javaw.exe"), QStringLiteral("lua"), QStringLiteral("lua.exe"),
        QStringLiteral("tclsh"), QStringLiteral("wish"),
        QStringLiteral("awk"), QStringLiteral("gawk"), QStringLiteral("sed"),
        // 下载器
        QStringLiteral("curl"), QStringLiteral("curl.exe"),
        QStringLiteral("wget"), QStringLiteral("wget.exe"),
        QStringLiteral("ftp"), QStringLiteral("ftp.exe"),
        QStringLiteral("tftp"), QStringLiteral("tftp.exe"),
    };
    return kBlocked.contains(baseNameLower);
}

// ============================================================================
// 脚本类扩展名
//
//  为什么要单独拦：用"默认程序"打开一个 .bat 等价于双击运行它。
//  用户以为在"查看文件"，实际在"执行代码"。这条规则把两者分开：
//  要看内容就显式用文本编辑器打开（也可以，但会走 launchApplication
//  并明确告诉你是在启动一个程序）。
// ============================================================================
bool SystemService::isScriptExtension(const QString &suffixLower)
{
    static const QStringList kScripts = {
        // Windows
        QStringLiteral("bat"), QStringLiteral("cmd"), QStringLiteral("ps1"),
        QStringLiteral("ps1xml"), QStringLiteral("psc1"), QStringLiteral("psd1"),
        QStringLiteral("vbs"), QStringLiteral("vbe"), QStringLiteral("js"),
        QStringLiteral("jse"), QStringLiteral("wsf"), QStringLiteral("wsh"),
        QStringLiteral("hta"), QStringLiteral("sct"), QStringLiteral("shb"),
        QStringLiteral("scr"), QStringLiteral("pif"), QStringLiteral("com"),
        QStringLiteral("lnk"), QStringLiteral("url"), QStringLiteral("reg"),
        QStringLiteral("cpl"), QStringLiteral("msc"), QStringLiteral("inf"),
        QStringLiteral("job"), QStringLiteral("application"),
        QStringLiteral("gadget"), QStringLiteral("diagcab"),
        // POSIX
        QStringLiteral("sh"), QStringLiteral("run"), QStringLiteral("desktop"),
        QStringLiteral("bash"), QStringLiteral("zsh"), QStringLiteral("service"),
        QStringLiteral("timer"), QStringLiteral("rules"),
    };
    return kScripts.contains(suffixLower);
}

bool SystemService::extensionAllowed(const QString &path, const QStringList &allowed)
{
    if (allowed.isEmpty()) {
        return true;
    }
    const QString suffix = QFileInfo(path).suffix().toLower();
    for (const QString &a : allowed) {
        if (suffix == a.trimmed().toLower().remove(QLatin1Char('.'))) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// 启动目标校验
// ============================================================================
SystemActionResult SystemService::validateLaunchTarget(const QString &path,
                                                       const LaunchOptions &options,
                                                       QString *resolved) const
{
    if (path.trimmed().isEmpty()) {
        return SystemActionResult::failure(ec(erc::InvalidArgument),
                                           tr("未提供要打开的目标。"));
    }

    // 第一步：走 PathGuard 全量检查（穿越、非法字符、保留名、UNC、白名单……）
    const PathCheckResult check = PathGuard::check(path, m_policy);
    if (!check.ok) {
        // 拒绝原因已由 PathGuard 给出中文说明，直接透传，不要自己重写，
        // 否则同一个问题在不同入口会有不同说法，用户无法对照排查。
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝启动目标：%1（%2）")
                    .arg(PathGuard::redact(path), check.message));
        return SystemActionResult::failure(check.errorCode, check.message);
    }

    const QString absolute = check.normalizedPath;
    if (resolved) {
        *resolved = absolute;
    }

    const QFileInfo info(absolute);

    // 第二步：解释器黑名单
    const QString baseLower = info.fileName().toLower();
    if (isInterpreterProgram(baseLower) && !options.allowInterpreter) {
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝启动解释器/终端：%1").arg(baseLower));
        return SystemActionResult::failure(
            ec(erc::OperationBlocked),
            tr("出于安全考虑，不允许启动命令解释器或脚本运行时（%1）。"
               "这类程序可以执行任意命令，无法纳入权限分级管理。").arg(info.fileName()));
    }

    // 第三步：系统目录下的可执行文件需要显式放开
    if (!options.allowSystemExecutable && PathGuard::isSystemLocation(absolute)) {
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝启动系统目录下的程序：%1")
                    .arg(PathGuard::redact(absolute)));
        return SystemActionResult::failure(
            ec(erc::OperationBlocked),
            tr("该程序位于系统目录中，未获授权不予启动。"));
    }

    return SystemActionResult::success();
}

// ============================================================================
// 4.5.1 启动应用程序
// ============================================================================
SystemActionResult SystemService::launchApplication(const QString &path,
                                                    const LaunchOptions &options)
{
    QString resolved;
    const SystemActionResult pre = validateLaunchTarget(path, options, &resolved);
    if (!pre.ok) {
        return pre;
    }

    const QFileInfo info(resolved);
    if (!info.exists()) {
        return SystemActionResult::failure(
            ec(erc::AppPathMissing),
            tr("找不到该程序：%1").arg(info.fileName()),
            resolved);
    }
    if (!info.isFile()) {
        return SystemActionResult::failure(
            ec(erc::InvalidArgument),
            tr("目标不是一个文件：%1").arg(info.fileName()),
            resolved);
    }
#ifndef Q_OS_WIN
    if (!info.isExecutable()) {
        return SystemActionResult::failure(
            ec(erc::PermissionDenied),
            tr("该文件没有执行权限：%1").arg(info.fileName()),
            resolved);
    }
#endif

    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    // 参数数组传递，不经过 shell —— 这是消除注入面的关键（SRS 7.3）
    const OpenTargetResult launch = adapter->launchProcess(resolved, options.arguments,
                                                           options.workingDirectory);
    if (!launch.ok) {
        DC_WARN(LogCat::System, QStringLiteral("启动程序失败：%1（%2）")
                    .arg(PathGuard::redact(resolved), launch.errorCode));
        return SystemActionResult::failure(
            launch.errorCode,
            tr("启动失败：%1").arg(launch.message));
    }

    DC_INFO(LogCat::System, QStringLiteral("已启动程序：%1").arg(PathGuard::redact(resolved)));
    return SystemActionResult::success();
}

// ============================================================================
// 按名称启动已安装程序
// ============================================================================
SystemActionResult SystemService::launchInstalledProgram(const QString &programName,
                                                         const LaunchOptions &options)
{
    const QString name = programName.trimmed();
    if (name.isEmpty()) {
        return SystemActionResult::failure(ec(erc::InvalidArgument), tr("未提供程序名称。"));
    }

    // 名称里出现路径分隔符，说明用户实际想指定路径 —— 转交完整路径流程，
    // 避免"名字"被用来偷偷指定路径而绕过路径校验。
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1String(".."))) {
        return launchApplication(name, options);
    }

    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    // PlatformAdapter 内部只在受信任的系统程序目录中查找，不搜索当前目录，
    // 从而避免"当前目录劫持"这一类经典攻击。
    const QString resolved = adapter->resolveInstalledProgram(name);
    if (resolved.isEmpty()) {
        return SystemActionResult::failure(
            ec(erc::AppIdNotFound),
            tr("未找到已安装的程序：%1。可以改用“打开”指令直接指定完整路径。").arg(name));
    }

    // 解析结果同样要过一遍全部检查，不因为"是在系统目录里找到的"就放行
    LaunchOptions effective = options;
    effective.allowSystemExecutable = true;  // 已确认来源可信，仅此一处放开
    return launchApplication(resolved, effective);
}

// ============================================================================
// 4.5.2 用默认程序打开文件
// ============================================================================
SystemActionResult SystemService::openPath(const QString &path, const OpenOptions &options)
{
    if (path.trimmed().isEmpty()) {
        return SystemActionResult::failure(ec(erc::InvalidArgument),
                                           tr("未提供要打开的文件。"));
    }

    // 无论是否要求白名单，PathGuard 的基础检查都必须过。
    // requireWhitelist=false 时放宽白名单，但穿越/非法字符/保留名/UNC 仍拦截。
    PathPolicy policy = m_policy;
    if (!options.requireWhitelist) {
        policy.requireWhitelist = false;
        policy.allowedRoots.clear();
    }

    const PathCheckResult check = PathGuard::check(path, policy);
    if (!check.ok) {
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝打开路径：%1（%2）")
                    .arg(PathGuard::redact(path), check.message));
        return SystemActionResult::failure(check.errorCode, check.message);
    }

    const QString absolute = check.normalizedPath;
    const QFileInfo info(absolute);

    if (!info.exists()) {
        return SystemActionResult::failure(
            ec(erc::PathNotFound),
            tr("找不到该文件或文件夹：%1").arg(info.fileName()),
            absolute);
    }

    if (!extensionAllowed(absolute, options.allowedExtensions)) {
        return SystemActionResult::failure(
            ec(erc::OperationBlocked),
            tr("该文件类型不在允许打开的范围内：.%1").arg(info.suffix()));
    }

    // 脚本文件不用"默认程序"打开 —— 否则等于变相执行脚本（见函数注释）
    const QString suffix = info.suffix().toLower();
    if (!suffix.isEmpty() && isScriptExtension(suffix)) {
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝以默认程序打开脚本类文件：.%1").arg(suffix));
        return SystemActionResult::failure(
            ec(erc::OperationBlocked),
            tr("出于安全考虑，不能用默认程序打开脚本或可执行类型文件（.%1）。"
               "如需查看内容，请用文本编辑器打开它。").arg(suffix));
    }

    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    const OpenTargetResult opened = adapter->openWithDefaultApplication(absolute);
    if (!opened.ok) {
        return SystemActionResult::failure(
            opened.errorCode,
            tr("打开失败：%1").arg(opened.message));
    }

    DC_INFO(LogCat::System, QStringLiteral("已用默认程序打开：%1").arg(PathGuard::redact(absolute)));
    return SystemActionResult::success();
}

// ============================================================================
// 4.5.3 在文件管理器中定位
// ============================================================================
SystemActionResult SystemService::revealInFileManager(const QString &path)
{
    const PathCheckResult check = PathGuard::check(path, m_policy);
    if (!check.ok) {
        return SystemActionResult::failure(check.errorCode, check.message);
    }

    const QString absolute = check.normalizedPath;
    if (!QFileInfo::exists(absolute)) {
        return SystemActionResult::failure(
            ec(erc::PathNotFound),
            tr("找不到该文件或文件夹：%1").arg(QFileInfo(absolute).fileName()));
    }

    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    const OpenTargetResult reveal = adapter->revealInFileManager(absolute);
    if (!reveal.ok) {
        return SystemActionResult::failure(
            reveal.errorCode,
            tr("定位失败：%1").arg(reveal.message));
    }

    DC_INFO(LogCat::System, QStringLiteral("已在文件管理器中定位：%1").arg(PathGuard::redact(absolute)));
    return SystemActionResult::success();
}

// ============================================================================
// 打开网址
// ============================================================================
SystemActionResult SystemService::openUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        return SystemActionResult::failure(ec(erc::InvalidArgument),
                                           tr("未提供要打开的网址。"));
    }

    const QUrl parsed(trimmed, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) {
        return SystemActionResult::failure(
            ec(erc::InvalidArgument),
            tr("网址格式无效：%1").arg(PathGuard::redact(trimmed)));
    }

    const QString scheme = parsed.scheme().toLower();
    // 只允许 http/https：file://、javascript:、ms-*、各类自定义协议
    // 都可能变成本地执行入口，一律拒绝（SRS 7.3）。
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        DC_WARN(LogCat::System, QStringLiteral("SystemService 拒绝打开非 http(s) 链接，scheme=%1").arg(scheme));
        return SystemActionResult::failure(
            ec(erc::NetworkInsecureScheme),
            tr("只允许打开 http 或 https 链接。"));
    }

    if (parsed.host().isEmpty()) {
        return SystemActionResult::failure(ec(erc::InvalidArgument),
                                           tr("网址缺少有效的主机名。"));
    }

    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    const OpenTargetResult opened = adapter->openWithDefaultApplication(parsed.toString());
    if (!opened.ok) {
        return SystemActionResult::failure(
            opened.errorCode,
            tr("打开链接失败：%1").arg(opened.message));
    }
    return SystemActionResult::success();
}

// ============================================================================
// 打开系统设置页
//
//  设计立场：我们不替用户改注册表、改开机项、改环境变量。
//  高风险操作要么在权限分级里明确授权后由本程序执行（可审计），
//  要么把用户带到系统界面由用户自己决定。这里实现的是后者。
// ============================================================================
SystemActionResult SystemService::openSystemSettings(SystemSettingsPage page)
{
    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }

    QString target;
    switch (page) {
    case SystemSettingsPage::EnvironmentVariables:
#ifdef Q_OS_WIN
        // ms-settings: 无法直接定位到环境变量页，用官方控制台入口代替
        target = QStringLiteral("SystemPropertiesAdvanced.exe");
#else
        target = QStringLiteral("gnome-control-center");
#endif
        break;
    case SystemSettingsPage::StartupApps:
#ifdef Q_OS_WIN
        target = QStringLiteral("ms-settings:startupapps");
#else
        target = QStringLiteral("gnome-session-properties");
#endif
        break;
    case SystemSettingsPage::Microphone:
#ifdef Q_OS_WIN
        target = QStringLiteral("ms-settings:privacy-microphone");
#else
        target = QStringLiteral("gnome-control-center");
#endif
        break;
    case SystemSettingsPage::Sound:
#ifdef Q_OS_WIN
        target = QStringLiteral("ms-settings:sound");
#else
        target = QStringLiteral("gnome-control-center");
#endif
        break;
    case SystemSettingsPage::Network:
#ifdef Q_OS_WIN
        target = QStringLiteral("ms-settings:network");
#else
        target = QStringLiteral("gnome-control-center");
#endif
        break;
    }

    const OpenTargetResult opened = adapter->openWithDefaultApplication(target);
    if (!opened.ok) {
        return SystemActionResult::failure(
            opened.errorCode,
            tr("无法自动打开系统设置页，请手动打开。"));
    }
    return SystemActionResult::success();
}

// ============================================================================
// 剪贴板
// ============================================================================
QString SystemService::clipboardText() const
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return QString();
    }
    const QString text = clipboard->text();
    // 上限保护：误复制一个巨大的文本文件不应把界面卡住。
    static constexpr int kMaxClipboardChars = 65536;
    if (text.size() > kMaxClipboardChars) {
        return text.left(kMaxClipboardChars);
    }
    return text;
}

SystemActionResult SystemService::setClipboardText(const QString &text)
{
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard) {
        return SystemActionResult::failure(ec(erc::NotSupported),
                                           tr("当前环境不支持剪贴板。"));
    }
    clipboard->setText(text);
    return SystemActionResult::success();
}

SystemActionResult SystemService::beep()
{
    PlatformAdapter *adapter = PlatformAdapter::instance();
    if (!adapter) {
        return SystemActionResult::failure(ec(erc::NotInitialized),
                                           tr("平台适配层未初始化。"));
    }
    adapter->beep();
    return SystemActionResult::success();
}

} // namespace dc
