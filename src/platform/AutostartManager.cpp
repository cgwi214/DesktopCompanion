// ============================================================================
//  AutostartManager.cpp — 开机启动实现
//
//  需求依据：SRS V1.1 4.12、4.4、6.2、7.3
//
//  实现要点：
//    1. 只用「用户级」位置，绝不碰 HKLM / /etc（SRS 4.12 明确要求）
//    2. 关闭时删除自己创建的条目；同时在启用时先做一次残留清理，
//       避免用户手工移动程序后留下指向旧路径的失效启动项
//    3. 所有写入都检查目标目录是否在用户可写范围内
// ============================================================================
#include "platform/AutostartManager.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/PathGuard.h"
#include "platform/PlatformAdapter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#  include <windows.h>
// IShellLinkW / IPersistFile / CLSID_ShellLink 都在 shlobj.h。
// windows.h 本身不带这些 —— 只 include windows.h 就会得到
// "IShellLinkW was not declared"。
#  include <shlobj.h>
#  include <objbase.h>
#endif

namespace dc {

// ---------------------------------------------------------------------------
// 启动项文件名 / 注册表值名 —— 集中定义，确保「启用」与「清理」用的是同一个名字
// ---------------------------------------------------------------------------
namespace {
const QString kAppDisplayName = QStringLiteral("DesktopCompanion");
const QString kStartupShortcutName = QStringLiteral("DesktopCompanion.lnk");
const QString kXdgDesktopFileName = QStringLiteral("desktop-companion.desktop");
const QString kRegistryValueName = QStringLiteral("DesktopCompanion");
const QString kRegistryRunKey =
    QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Run");

// 延迟启动参数名
const QString kArgStartMinimized = QStringLiteral("--start-minimized");
const QString kArgStartHidden = QStringLiteral("--start-hidden");
const QString kArgDelay = QStringLiteral("--startup-delay");
} // namespace

QString autostartMethodToString(AutostartMethod method)
{
    switch (method) {
    case AutostartMethod::WindowsStartupFolder: return QStringLiteral("windows_startup_folder");
    case AutostartMethod::WindowsRegistryRun:   return QStringLiteral("windows_registry_run");
    case AutostartMethod::XdgAutostart:         return QStringLiteral("xdg_autostart");
    case AutostartMethod::LaunchAgent:          return QStringLiteral("launch_agent");
    case AutostartMethod::NotSupported:         break;
    }
    return QStringLiteral("not_supported");
}

// ===========================================================================
//  AutostartManager
// ===========================================================================
AutostartManager::AutostartManager(PlatformAdapter *adapter, QObject *parent)
    : QObject(parent)
    , m_adapter(adapter)
{
}

AutostartManager::~AutostartManager() = default;

// ---------------------------------------------------------------------------
// 方法选择（SRS 4.12 的优先顺序）
// ---------------------------------------------------------------------------
AutostartMethod AutostartManager::selectMethod()
{
#ifdef Q_OS_WIN
    // 优先启动文件夹：用户可以直观地看到并手工删除，符合「可解释」原则
    if (isAvailable(AutostartMethod::WindowsStartupFolder))
        return AutostartMethod::WindowsStartupFolder;
    // 回退到用户级注册表 Run 键（仍是 HKCU，不需要管理员权限）
    if (isAvailable(AutostartMethod::WindowsRegistryRun))
        return AutostartMethod::WindowsRegistryRun;
    return AutostartMethod::NotSupported;
#elif defined(Q_OS_LINUX)
    if (isAvailable(AutostartMethod::XdgAutostart))
        return AutostartMethod::XdgAutostart;
    return AutostartMethod::NotSupported;
#else
    return AutostartMethod::NotSupported;
#endif
}

bool AutostartManager::isAvailable(AutostartMethod method, QString *reason)
{
    auto fail = [reason](const QString &r) {
        if (reason) *reason = r;
        return false;
    };

    switch (method) {
    case AutostartMethod::WindowsStartupFolder: {
#ifdef Q_OS_WIN
        const QString appData = qEnvironmentVariable("APPDATA");
        if (appData.isEmpty())
            return fail(QStringLiteral("未能确定当前用户的 AppData 目录。"));

        const QString startupDir =
            appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");

        // 目录不存在说明系统结构异常；尝试创建
        QDir dir(startupDir);
        if (!dir.exists()) {
            if (!dir.mkpath(QStringLiteral(".")))
                return fail(QStringLiteral("无法创建启动文件夹：%1").arg(startupDir));
        }

        // 可写性检查
        const QFileInfo info(startupDir);
        if (!info.isWritable())
            return fail(QStringLiteral("启动文件夹不可写：%1").arg(startupDir));

        if (reason) reason->clear();
        return true;
#else
        return fail(QStringLiteral("仅 Windows 支持启动文件夹方式。"));
#endif
    }

    case AutostartMethod::WindowsRegistryRun: {
#ifdef Q_OS_WIN
        // 只检查 HKCU 的可写性，不真正写入
        HKEY key = nullptr;
        const LONG rc = ::RegOpenKeyExW(
            HKEY_CURRENT_USER,
            reinterpret_cast<const wchar_t *>(kRegistryRunKey.utf16()),
            0,
            KEY_READ | KEY_WRITE,
            &key);

        if (rc != ERROR_SUCCESS) {
            return fail(QStringLiteral("无法打开当前用户的启动注册表项（错误码 %1）。")
                            .arg(rc));
        }
        ::RegCloseKey(key);
        if (reason) reason->clear();
        return true;
#else
        return fail(QStringLiteral("仅 Windows 支持注册表启动项。"));
#endif
    }

    case AutostartMethod::XdgAutostart: {
#ifndef Q_OS_WIN
        const QString configHome =
            qEnvironmentVariable("XDG_CONFIG_HOME",
                                 QDir::homePath() + QStringLiteral("/.config"));
        const QString autostartDir = configHome + QStringLiteral("/autostart");

        QDir dir(autostartDir);
        if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
            return fail(QStringLiteral("无法创建 XDG autostart 目录：%1").arg(autostartDir));

        if (!QFileInfo(autostartDir).isWritable())
            return fail(QStringLiteral("XDG autostart 目录不可写：%1").arg(autostartDir));

        if (reason) reason->clear();
        return true;
#else
        return fail(QStringLiteral("仅 Linux 支持 XDG Autostart。"));
#endif
    }

    case AutostartMethod::LaunchAgent:
        return fail(QStringLiteral("当前版本未实现 macOS 开机启动。"));

    case AutostartMethod::NotSupported:
        break;
    }

    return fail(QStringLiteral("当前系统不支持开机启动设置。"));
}

// ---------------------------------------------------------------------------
// 命令行构造
//
//  注意：Windows 启动文件夹放的是 .lnk 快捷方式（由 Shell 链接对象处理），
//        注册表 Run 放的是命令行字符串 —— 后者必须正确加引号，
//        否则路径中含空格时会启动失败。这里用统一的双引号包裹并用
//        双引号转义，不依赖 shell 语义。
// ---------------------------------------------------------------------------
QStringList AutostartManager::buildArguments(const AutostartOptions &options)
{
    QStringList args;
    if (options.startMinimized)
        args << kArgStartMinimized;
    if (options.startHidden)
        args << kArgStartHidden;
    if (options.delaySeconds > 0)
        args << kArgDelay << QString::number(options.delaySeconds);
    return args;
}

QString AutostartManager::buildCommandLine(const AutostartOptions &options)
{
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    // Windows 命令行引号规则：路径含空格或特殊字符时必须加引号，
    // 内部的双引号需要转义。这里路径由程序自身路径得到，不含用户输入，
    // 但仍按完整规则处理以保持严谨。
    auto quote = [](const QString &s) -> QString {
        if (s.isEmpty())
            return QStringLiteral("\"\"");
        if (!s.contains(QLatin1Char(' ')) && !s.contains(QLatin1Char('\t'))
            && !s.contains(QLatin1Char('"'))) {
            return s;
        }
        QString escaped = s;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QStringLiteral("\"") + escaped + QStringLiteral("\"");
    };

    QString cmd = quote(exe);
    for (const QString &arg : buildArguments(options))
        cmd += QLatin1Char(' ') + quote(arg);

    return cmd;
}

// ---------------------------------------------------------------------------
// status
// ---------------------------------------------------------------------------
AutostartStatus AutostartManager::status() const
{
    switch (selectMethod()) {
    case AutostartMethod::WindowsStartupFolder:
        return statusWindowsStartupFolder();
    case AutostartMethod::WindowsRegistryRun:
        return statusWindowsRegistryRun();
    case AutostartMethod::XdgAutostart:
        return statusXdgAutostart();
    default:
        break;
    }

    AutostartStatus s;
    s.methodAvailable = false;
    s.method = AutostartMethod::NotSupported;
    isAvailable(AutostartMethod::NotSupported, &s.unavailableReason);
    if (s.unavailableReason.isEmpty()) {
        s.unavailableReason = QStringLiteral(
            "当前系统不支持开机启动设置。你仍可手工把程序快捷方式放入系统启动项。");
    }
    return s;
}

AutostartStatus AutostartManager::statusWindowsStartupFolder() const
{
    AutostartStatus s;
    s.method = AutostartMethod::WindowsStartupFolder;
    s.methodAvailable = true;

#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString startupDir =
        appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");
    const QString linkPath = startupDir + QLatin1Char('/') + kStartupShortcutName;

    s.location = linkPath;
    s.enabled = QFile::exists(linkPath);

    // 读取 .lnk 内容判断是否指向当前程序。
    // 这里不解析 lnk 二进制格式，而是通过搜索文件内容中的路径字符串做
    // 保守判断：如果找不到当前可比对的形态，就标记为「未知」而非「匹配」。
    if (s.enabled) {
        QFile f(linkPath);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.readAll();
            f.close();
            const QString exeName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
            s.matchesCurrentExecutable = data.contains(exeName.toUtf8());
        }
    }
#else
    s.methodAvailable = false;
    s.unavailableReason = QStringLiteral("仅 Windows 支持启动文件夹方式。");
#endif

    return s;
}

AutostartStatus AutostartManager::statusWindowsRegistryRun() const
{
    AutostartStatus s;
    s.method = AutostartMethod::WindowsRegistryRun;
    s.methodAvailable = true;
    s.location = QStringLiteral("HKEY_CURRENT_USER\\") + kRegistryRunKey
                 + QStringLiteral(" [") + kRegistryValueName + QStringLiteral("]");

#ifdef Q_OS_WIN
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        reinterpret_cast<const wchar_t *>(kRegistryRunKey.utf16()),
                        0, KEY_READ, &key) != ERROR_SUCCESS) {
        s.enabled = false;
        return s;
    }

    wchar_t buffer[2048] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;

    const LONG rc = ::RegQueryValueExW(
        key,
        reinterpret_cast<const wchar_t *>(kRegistryValueName.utf16()),
        nullptr, &type,
        reinterpret_cast<LPBYTE>(buffer), &size);

    ::RegCloseKey(key);

    if (rc == ERROR_SUCCESS && type == REG_SZ) {
        s.enabled = true;
        s.registeredCommand = QString::fromWCharArray(buffer);

        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        s.matchesCurrentExecutable = s.registeredCommand.contains(exe, Qt::CaseInsensitive);
    }
#else
    s.methodAvailable = false;
    s.unavailableReason = QStringLiteral("仅 Windows 支持注册表启动项。");
#endif

    return s;
}

AutostartStatus AutostartManager::statusXdgAutostart() const
{
    AutostartStatus s;
    s.method = AutostartMethod::XdgAutostart;
    s.methodAvailable = true;

#ifndef Q_OS_WIN
    const QString configHome =
        qEnvironmentVariable("XDG_CONFIG_HOME",
                             QDir::homePath() + QStringLiteral("/.config"));
    const QString filePath = configHome + QStringLiteral("/autostart/") + kXdgDesktopFileName;

    s.location = filePath;
    s.enabled = QFile::exists(filePath);

    if (s.enabled) {
        QFile f(filePath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll());
            f.close();

            const QString exe = QCoreApplication::applicationFilePath();

            // 同时检查 Hidden=true —— 用户可能用这个字段禁用了启动
            static const QRegularExpression hiddenRe(
                QStringLiteral("^\\s*Hidden\\s*=\\s*true\\s*$"),
                QRegularExpression::MultilineOption
                    | QRegularExpression::CaseInsensitiveOption);
            if (hiddenRe.match(content).hasMatch())
                s.enabled = false;

            s.matchesCurrentExecutable = content.contains(exe);
        }
    }
#else
    s.methodAvailable = false;
    s.unavailableReason = QStringLiteral("仅 Linux 支持 XDG Autostart。");
#endif

    return s;
}

// ---------------------------------------------------------------------------
// enable
// ---------------------------------------------------------------------------
Result AutostartManager::enable(const AutostartOptions &options)
{
    const AutostartMethod method = selectMethod();

    switch (method) {
    case AutostartMethod::WindowsStartupFolder:
        return enableWindowsStartupFolder(options);
    case AutostartMethod::WindowsRegistryRun:
        return enableWindowsRegistryRun(options);
    case AutostartMethod::XdgAutostart:
        return enableXdgAutostart(options);
    default:
        break;
    }

    QString reason;
    isAvailable(method, &reason);
    return Result::failure(erc::PlatformAutostartFailed,
                           reason.isEmpty()
                               ? erc::describe(erc::PlatformAutostartFailed)
                               : reason);
}

// ---------------------------------------------------------------------------
//  Windows：启动文件夹方式
//
//  技术选择说明：
//    创建真正的 .lnk 需要 COM（IShellLink + IPersistFile）。
//    为了避免引入 COM 初始化的复杂度与失败面，这里采用「复制可执行文件的
//    快捷方式描述文件」的方式实现是不够的（.lnk 是二进制格式）。
//    因此本实现使用 Qt 的方式：写入一个 .lnk 替代方案 —— 一个
//    内容为该程序命令行的小型 .url/.cmd？—— 不行，都不是好的做法。
//
//  最终方案：
//    Windows 启动文件夹方式通过 COM 创建标准 .lnk。
//    COM 调用被限制在本文件的局部函数内，失败时自动回退到注册表方式。
// ---------------------------------------------------------------------------
Result AutostartManager::enableWindowsStartupFolder(const AutostartOptions &options)
{
#ifdef Q_OS_WIN
    QString reason;
    if (!isAvailable(AutostartMethod::WindowsStartupFolder, &reason)) {
        return Result::failure(erc::PlatformAutostartFailed, reason);
    }

    // 先清理可能存在的旧条目（程序位置可能已变动）
    disableWindowsStartupFolder();

    const QString exe = QCoreApplication::applicationFilePath();
    const QString arguments = buildArguments(options).join(QLatin1Char(' '));

    const QString appData = qEnvironmentVariable("APPDATA");
    const QString startupDir =
        appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");
    const QString linkPath = QDir::toNativeSeparators(
        startupDir + QLatin1Char('/') + kStartupShortcutName);

    // --- 使用 COM 创建 .lnk ---
    // 必须 CoInitialize；若调用线程已初始化会返回 S_FALSE，属正常情况。
    const HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool created = false;
    {
        IShellLinkW *shellLink = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellLinkW,
                                        reinterpret_cast<void **>(&shellLink));
        if (SUCCEEDED(hr) && shellLink) {
            const QString nativeExe = QDir::toNativeSeparators(exe);
            shellLink->SetPath(reinterpret_cast<const wchar_t *>(nativeExe.utf16()));
            shellLink->SetArguments(reinterpret_cast<const wchar_t *>(arguments.utf16()));
            shellLink->SetDescription(L"Desktop Companion 桌面宠物助手");
            shellLink->SetWorkingDirectory(
                reinterpret_cast<const wchar_t *>(
                    QDir::toNativeSeparators(QCoreApplication::applicationDirPath()).utf16()));

            IPersistFile *persistFile = nullptr;
            hr = shellLink->QueryInterface(IID_IPersistFile,
                                           reinterpret_cast<void **>(&persistFile));
            if (SUCCEEDED(hr) && persistFile) {
                const QString nativeLink = QDir::toNativeSeparators(linkPath);
                hr = persistFile->Save(
                    reinterpret_cast<const wchar_t *>(nativeLink.utf16()), TRUE);
                if (SUCCEEDED(hr))
                    created = true;
                persistFile->Release();
            }
            shellLink->Release();
        }
    }

    if (SUCCEEDED(coInit))
        ::CoUninitialize();

    if (created) {
        DC_INFO(LogCat::Platform,
                QStringLiteral("已创建开机启动快捷方式：%1").arg(PathGuard::redact(linkPath)));
        return Result::success();
    }

    // COM 失败 -> 回退到注册表方式（SRS 6.2：必要时提供替代实现）
    DC_WARN(LogCat::Platform,
            QStringLiteral("创建启动文件夹快捷方式失败，回退到当前用户注册表启动项。"));
    return enableWindowsRegistryRun(options);
#else
    Q_UNUSED(options)
    return Result::failure(erc::PlatformUnsupported,
                           QStringLiteral("仅 Windows 支持启动文件夹方式。"));
#endif
}

// ---------------------------------------------------------------------------
//  Windows：注册表 Run 键方式（HKCU，不需要管理员权限）
// ---------------------------------------------------------------------------
Result AutostartManager::enableWindowsRegistryRun(const AutostartOptions &options)
{
#ifdef Q_OS_WIN
    QString reason;
    if (!isAvailable(AutostartMethod::WindowsRegistryRun, &reason)) {
        return Result::failure(erc::PlatformAutostartFailed, reason);
    }

    const QString commandLine = buildCommandLine(options);

    HKEY key = nullptr;
    const LONG openRc = ::RegCreateKeyExW(
        HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t *>(kRegistryRunKey.utf16()),
        0, nullptr, 0, KEY_WRITE,
        nullptr, &key, nullptr);

    if (openRc != ERROR_SUCCESS) {
        return Result::failure(
            erc::PlatformAutostartFailed,
            QStringLiteral("无法打开当前用户的启动注册表项（错误码 %1）。").arg(openRc));
    }

    // 注册表只写当前用户（HKCU）。绝不写 HKLM —— 那会影响所有用户，
    // 且通常需要管理员权限，与本项目"不静默提权"的原则冲突（SRS 4.4）。
    const QString keyName = kRegistryValueName;
    const std::wstring wValue = commandLine.toStdWString();

    const LONG setRc = ::RegSetValueExW(
        key,
        reinterpret_cast<const wchar_t *>(keyName.utf16()),
        0, REG_SZ,
        reinterpret_cast<const BYTE *>(wValue.c_str()),
        static_cast<DWORD>((wValue.size() + 1) * sizeof(wchar_t)));

    ::RegCloseKey(key);

    if (setRc != ERROR_SUCCESS) {
        return Result::failure(
            erc::PlatformAutostartFailed,
            QStringLiteral("写入开机启动注册表项失败（错误码 %1）。").arg(setRc));
    }

    DC_INFO(LogCat::Platform,
            QStringLiteral("已写入开机启动注册表项（HKCU）：%1").arg(commandLine));
    return Result::success();
#else
    Q_UNUSED(options)
    return Result::failure(erc::PlatformUnsupported,
                           QStringLiteral("仅 Windows 支持注册表启动项。"));
#endif
}

// ---------------------------------------------------------------------------
//  Linux：XDG Autostart
// ---------------------------------------------------------------------------
Result AutostartManager::enableXdgAutostart(const AutostartOptions &options)
{
#ifndef Q_OS_WIN
    QString reason;
    if (!isAvailable(AutostartMethod::XdgAutostart, &reason)) {
        return Result::failure(erc::PlatformAutostartFailed, reason);
    }

    const QString configHome =
        qEnvironmentVariable("XDG_CONFIG_HOME",
                             QDir::homePath() + QStringLiteral("/.config"));
    const QString dirPath = configHome + QStringLiteral("/autostart");
    const QString filePath = dirPath + QLatin1Char('/') + kXdgDesktopFileName;

    const QString exe = QCoreApplication::applicationFilePath();

    // Exec 字段：按 Desktop Entry 规范，可执行文件路径需要引号转义，
    // 参数中的 % 需写成 %%。这里路径来自程序自身，安全但仍按规范处理。
    auto execQuote = [](const QString &s) -> QString {
        QString out = s;
        out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        out.replace(QLatin1Char('`'), QStringLiteral("\\`"));
        out.replace(QLatin1Char('$'), QStringLiteral("\\$"));
        out.replace(QLatin1Char('%'), QStringLiteral("%%"));
        return QStringLiteral("\"") + out + QStringLiteral("\"");
    };

    QString execLine = execQuote(exe);
    for (const QString &arg : buildArguments(options))
        execLine += QLatin1Char(' ') + execQuote(arg);

    // 延迟启动通过 X-GNOME-Autostart-Delay（GNOME 支持）与自带的 --startup-delay
    // 双重保障，其它桌面环境至少能用程序自身实现的延迟。
    QStringList lines;
    lines << QStringLiteral("[Desktop Entry]")
          << QStringLiteral("Type=Application")
          << QStringLiteral("Version=1.0")
          << QStringLiteral("Name=Desktop Companion")
          << QStringLiteral("Name[zh_CN]=桌面宠物助手")
          << QStringLiteral("Name[ja]=デスクトップコンパニオン")
          << QStringLiteral("Comment=轻量级桌面宠物助手")
          << QStringLiteral("Comment[en]=Lightweight desktop pet assistant")
          << QStringLiteral("Exec=") + execLine
          << QStringLiteral("Icon=desktop-companion")
          << QStringLiteral("Terminal=false")
          << QStringLiteral("Categories=Utility;")
          << QStringLiteral("X-GNOME-Autostart-enabled=true")
          << QStringLiteral("# 由 Desktop Companion 自动生成；"
                           "关闭开机启动时会自动删除本文件。");

    if (options.delaySeconds > 0) {
        lines << QStringLiteral("X-GNOME-Autostart-Delay=")
                     + QString::number(options.delaySeconds);
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return Result::failure(
            erc::PlatformAutostartFailed,
            QStringLiteral("无法写入 autostart 文件：%1").arg(file.errorString()));
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    for (const QString &line : lines)
        stream << line << '\n';
    stream.flush();
    file.close();

    // Desktop Entry 规范要求 .desktop 文件可执行
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                        | QFile::ReadGroup | QFile::ExeGroup
                        | QFile::ReadOther | QFile::ExeOther);

    DC_INFO(LogCat::Platform,
            QStringLiteral("已创建 XDG autostart 文件：%1").arg(PathGuard::redact(filePath)));
    return Result::success();
#else
    Q_UNUSED(options)
    return Result::failure(erc::PlatformUnsupported,
                           QStringLiteral("仅 Linux 支持 XDG Autostart。"));
#endif
}

// ---------------------------------------------------------------------------
// disable
// ---------------------------------------------------------------------------
Result AutostartManager::disable()
{
    // 关闭时把两种方式都清一遍：用户可能在两次启用之间切换过实现方式，
    // 或者手工创建了另一种条目。全部清理才能保证「关闭就是真的关闭」。
    Result r1 = disableWindowsStartupFolder();
    Result r2 = disableWindowsRegistryRun();
    Result r3 = disableXdgAutostart();

    // 只要没有「失败且确实存在需要删除的东西」，就视为成功
    if (r1.ok && r2.ok && r3.ok)
        return Result::success();

    const QString detail = QStringLiteral("startup_folder=%1 registry=%2 xdg=%3")
                               .arg(r1.ok ? QStringLiteral("ok") : r1.detail)
                               .arg(r2.ok ? QStringLiteral("ok") : r2.detail)
                               .arg(r3.ok ? QStringLiteral("ok") : r3.detail);

    return Result::failure(erc::PlatformAutostartFailed,
                           erc::describe(erc::PlatformAutostartFailed), detail);
}

Result AutostartManager::disableWindowsStartupFolder()
{
#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty())
        return Result::success();

    const QString startupDir =
        appData + QStringLiteral("/Microsoft/Windows/Start Menu/Programs/Startup");
    const QString linkPath = startupDir + QLatin1Char('/') + kStartupShortcutName;

    if (!QFile::exists(linkPath))
        return Result::success();   // 本来就不存在，视为成功

    if (!QFile::remove(linkPath)) {
        return Result::failure(
            erc::PlatformAutostartFailed,
            QStringLiteral("无法删除开机启动快捷方式：%1").arg(PathGuard::redact(linkPath)));
    }

    DC_INFO(LogCat::Platform, QStringLiteral("已删除开机启动快捷方式：%1")
                                  .arg(PathGuard::redact(linkPath)));
#endif
    return Result::success();
}

Result AutostartManager::disableWindowsRegistryRun()
{
#ifdef Q_OS_WIN
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        reinterpret_cast<const wchar_t *>(kRegistryRunKey.utf16()),
                        0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return Result::success();   // 键不存在，无需清理
    }

    const LONG rc = ::RegDeleteValueW(
        key, reinterpret_cast<const wchar_t *>(kRegistryValueName.utf16()));
    ::RegCloseKey(key);

    // ERROR_FILE_NOT_FOUND 说明本来就没有，属于成功
    if (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) {
        if (rc == ERROR_SUCCESS) {
            DC_INFO(LogCat::Platform,
                    QStringLiteral("已删除开机启动注册表项（HKCU）"));
        }
        return Result::success();
    }

    return Result::failure(
        erc::PlatformAutostartFailed,
        QStringLiteral("无法删除开机启动注册表项（错误码 %1）。").arg(rc));
#else
    return Result::success();
#endif
}

Result AutostartManager::disableXdgAutostart()
{
#ifndef Q_OS_WIN
    const QString configHome =
        qEnvironmentVariable("XDG_CONFIG_HOME",
                             QDir::homePath() + QStringLiteral("/.config"));
    const QString filePath =
        configHome + QStringLiteral("/autostart/") + kXdgDesktopFileName;

    if (!QFile::exists(filePath))
        return Result::success();

    if (!QFile::remove(filePath)) {
        return Result::failure(
            erc::PlatformAutostartFailed,
            QStringLiteral("无法删除 autostart 文件：%1").arg(PathGuard::redact(filePath)));
    }

    DC_INFO(LogCat::Platform, QStringLiteral("已删除 XDG autostart 文件"));
#endif
    return Result::success();
}

// ---------------------------------------------------------------------------
// update
// ---------------------------------------------------------------------------
Result AutostartManager::update(const AutostartOptions &options)
{
    const AutostartStatus s = status();
    if (!s.enabled)
        return Result::success();   // 未启用则无需更新

    // 重新写入即可（各 enable* 会先清理旧条目）
    return enable(options);
}

// ---------------------------------------------------------------------------
// 残留清理
// ---------------------------------------------------------------------------
void AutostartManager::cleanupStaleEntries()
{
    // 若当前程序不是当初注册的那个可执行文件（用户移动了程序目录），
    // 说明残留项已失效，此时清掉它，避免用户看到「开机启动开了但没效果」。
    const AutostartStatus s = status();
    if (s.enabled && !s.matchesCurrentExecutable) {
        DC_WARN(LogCat::Platform,
                QStringLiteral("检测到指向旧路径的开机启动项（%1），将清理。")
                    .arg(s.location));
        disable();
    }
}

} // namespace dc
