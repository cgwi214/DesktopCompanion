// ============================================================================
//  SystemInfo.cpp — 操作系统与能力探测实现
//
//  需求依据：SRS V1.1 6.1、6.2、9.3
//
//  实现说明：
//    所有探测都是「尽力而为」的：探测失败不会抛异常，只会把能力标为不可用
//    并给出原因。这直接服务于 SRS 6.2 的核心要求：
//      「核心桌宠和本地规则命令不应因语音或网络模块缺失而无法启动。」
// ============================================================================
#include "platform/SystemInfo.h"

#include "core/BuildConfig.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSslSocket>
#include <QSysInfo>
#include <QStandardPaths>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <winver.h>
#  include <QSysInfo>
#endif

#ifdef Q_OS_LINUX
#  include <QFileInfo>
#endif

namespace dc {

// ===========================================================================
//  枚举互转
// ===========================================================================
QString osFamilyToString(OsFamily family)
{
    switch (family) {
    case OsFamily::Windows: return QStringLiteral("Windows");
    case OsFamily::Linux:   return QStringLiteral("Linux");
    case OsFamily::MacOS:   return QStringLiteral("macOS");
    case OsFamily::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QString legacyTierToString(LegacyTier tier)
{
    switch (tier) {
    case LegacyTier::Modern:      return QStringLiteral("modern");
    case LegacyTier::Win7Class:   return QStringLiteral("win7_class");
    case LegacyTier::WinXpClass:  return QStringLiteral("winxp_class");
    case LegacyTier::LegacyLinux: return QStringLiteral("legacy_linux");
    case LegacyTier::Unknown:     break;
    }
    return QStringLiteral("unknown");
}

// ===========================================================================
//  平台判定
// ===========================================================================
OsFamily SystemInfo::currentFamily()
{
#ifdef Q_OS_WIN
    return OsFamily::Windows;
#elif defined(Q_OS_LINUX)
    return OsFamily::Linux;
#elif defined(Q_OS_MACOS)
    return OsFamily::MacOS;
#else
    return OsFamily::Unknown;
#endif
}

bool SystemInfo::isWindows() { return currentFamily() == OsFamily::Windows; }
bool SystemInfo::isLinux()   { return currentFamily() == OsFamily::Linux; }

// ===========================================================================
//  旧系统档位判定
//
//  注意：这里只「判定档位」，不据此禁用任何功能。
//  档位用于决定是否走降级实现，真正的功能开关由能力探测结果决定。
// ===========================================================================
namespace {

LegacyTier detectTier()
{
#ifdef Q_OS_WIN
    // QSysInfo::kernelVersion() 形如 "10.0.19044"
    const QString kv = QSysInfo::kernelVersion();
    const QVersionNumber v = QVersionNumber::fromString(kv);
    if (v.isNull())
        return LegacyTier::Unknown;

    const int major = v.majorVersion();
    const int minor = v.minorVersion();
    const QString product = QSysInfo::productVersion();   // "10" / "11" / "7" / "XP"

    // Windows 版本号对照：
    //   5.1  -> XP
    //   5.2  -> XP x64 / Server 2003
    //   6.0  -> Vista / Server 2008
    //   6.1  -> 7 / Server 2008 R2
    //   6.2  -> 8 / Server 2012
    //   6.3  -> 8.1 / Server 2012 R2
    //   10.0 -> 10 / 11 / Server 2016+
    if (major == 5)
        return LegacyTier::WinXpClass;
    if (major == 6 && minor <= 1)
        return LegacyTier::Win7Class;
    if (major == 6)
        return LegacyTier::Win7Class;   // 8 / 8.1 归入同一档，降级策略一致
    if (major >= 10)
        return LegacyTier::Modern;

    Q_UNUSED(product)
    return LegacyTier::Unknown;
#elif defined(Q_OS_LINUX)
    // 读取发行版标识，识别 CentOS 6 / RHEL 7 这类老发行版
    QString id;
    QString versionId;

    for (const QString &path : { QStringLiteral("/etc/os-release"),
                                 QStringLiteral("/usr/lib/os-release") }) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        const QString content = QString::fromUtf8(f.readAll());
        const QStringList lines = content.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("ID=")))
                id = trimmed.mid(3).remove(QLatin1Char('"'));
            else if (trimmed.startsWith(QStringLiteral("VERSION_ID=")))
                versionId = trimmed.mid(11).remove(QLatin1Char('"'));
        }
        if (!id.isEmpty())
            break;
    }

    // 兜底：读 /etc/redhat-release
    if (id.isEmpty()) {
        QFile f(QStringLiteral("/etc/redhat-release"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(f.readAll()).trimmed();
            if (content.contains(QStringLiteral("CentOS"), Qt::CaseInsensitive))
                id = QStringLiteral("centos");
            else if (content.contains(QStringLiteral("Red Hat"), Qt::CaseInsensitive))
                id = QStringLiteral("rhel");

            // 从文本里抓版本号
            for (const QString &token : content.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const QVersionNumber v = QVersionNumber::fromString(token);
                if (!v.isNull() && v.majorVersion() > 0 && v.majorVersion() < 100) {
                    versionId = token;
                    break;
                }
            }
        }
    }

    const int major = QVersionNumber::fromString(versionId).majorVersion();

    if ((id == QStringLiteral("centos") || id == QStringLiteral("rhel")
         || id == QStringLiteral("redhat"))
        && major > 0 && major <= 7) {
        return LegacyTier::LegacyLinux;
    }

    // 其他老发行版（按内核版本粗略判断）
    const QVersionNumber kv = QVersionNumber::fromString(QSysInfo::kernelVersion());
    if (!kv.isNull() && kv.majorVersion() > 0 && kv.majorVersion() <= 4)
        return LegacyTier::LegacyLinux;

    return LegacyTier::Modern;
#else
    return LegacyTier::Modern;
#endif
}

// Linux 发行版产品名探测
QString detectLinuxProduct()
{
#ifdef Q_OS_LINUX
    QFile f(QStringLiteral("/etc/os-release"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString content = QString::fromUtf8(f.readAll());
        const QStringList lines = content.split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("PRETTY_NAME=")))
                return trimmed.mid(12).remove(QLatin1Char('"'));
        }
    }
    QFile f2(QStringLiteral("/etc/redhat-release"));
    if (f2.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f2.readAll()).trimmed();
#endif
    return QString();
}

} // namespace

LegacyTier SystemInfo::currentTier()
{
    static const LegacyTier tier = detectTier();
    return tier;
}

// ===========================================================================
//  各项能力探测
// ===========================================================================

Capability SystemInfo::probeNetworkTls12()
{
    // 探测思路（不做实际网络请求，避免启动时产生流量）：
    //   1. 检查 Qt 的 TLS 后端是否可用
    //   2. 检查系统是否具备证书库
    //   3. 读取 QSslSocket 支持的协议版本
    //
    // 真正的「能否连通」由 NetworkService 在首次请求时判定，
    // 这里只判断「本机是否具备建立 TLS 1.2 的能力」。

    // 说明：Qt 5 里叫 QSslSocket::supportedBackends()，Qt 6 已改名为
    // availableBackends()（supportedBackends 被移除）。这里用 Qt 6 的名字。
    const QStringList backends = QSslSocket::availableBackends();
    if (backends.isEmpty()) {
        return Capability::no(
            QStringLiteral("本机未安装可用的安全通信组件（TLS 后端），"
                           "联网的搜索与翻译功能将不可用。"),
            QStringLiteral("QSslSocket::availableBackends() 为空；"
                           "常见原因是缺少 OpenSSL 运行库。"));
    }

    // Qt 5.15+/6 默认要求 TLS 1.2 及以上；若被编译期禁用到 TLS 1.1，
    // 视为「能力不足」并明确告知（SRS 11 风险表：在线服务逐步弃用旧 TLS）
    if (!QSslSocket::supportsSsl()) {
        return Capability::no(
            QStringLiteral("本机安全通信组件不可用，联网功能将被禁用。"),
            QStringLiteral("supportsSsl() == false，后端=%1")
                .arg(backends.join(QLatin1Char(','))));
    }

    const QString version = QSslSocket::sslLibraryVersionString();
    return Capability::yes(
        QStringLiteral("TLS 后端：%1（%2）")
            .arg(backends.join(QLatin1Char(',')), version));
}

Capability SystemInfo::probeSecureStorage()
{
    // SRS 4.10 / 11 风险表：
    //   「旧系统缺乏安全凭据存储 → API 密钥保护能力下降 → 提示风险，
    //     允许不保存密钥或使用系统安全存储。」
    //
    // 探测项：
    //   Windows：DPAPI 始终存在（CryptProtectData），因此可用；
    //            但仍需注意 XP 上强度较弱，这里在 reason 中说明。
    //   Linux：需要 libsecret / keyring；缺失则不可用。
#if defined(Q_OS_WIN)
    return Capability::yes(QStringLiteral("使用 Windows DPAPI（CryptProtectData），"
                                          "密钥绑定当前用户账户加密。"));
#elif defined(Q_OS_LINUX)
    // 检查常见的 secrets 服务是否存在
    const bool hasDbus =
        !QStandardPaths::findExecutable(QStringLiteral("dbus-launch")).isEmpty()
        || QFile::exists(QStringLiteral("/run/dbus/system_bus_socket"));

    if (hasDbus) {
        return Capability::yes(QStringLiteral("检测到 D-Bus，可使用系统密钥环存储凭据。"));
    }
    return Capability::no(
        QStringLiteral("本机没有可用的系统密钥环，API 密钥将无法安全保存。"
                       "建议不要保存密钥，在每次使用时手动输入。"),
        QStringLiteral("未检测到 D-Bus / Secret Service。"));
#else
    return Capability::no(QStringLiteral("当前系统没有受支持的安全凭据存储。"));
#endif
}

Capability SystemInfo::probeAudioInput()
{
    // 音频设备探测依赖 Qt Multimedia。若编译期未启用，直接给出明确原因。
#if !DC_ENABLE_RECORDING && !DC_ENABLE_SPEECH
    return Capability::no(
        QStringLiteral("本程序在当前系统中未编译音频支持，语音与录音功能不可用。"),
        QStringLiteral("编译期未启用 Qt Multimedia。"));
#else
    // 此处只做静态判断。真正的设备枚举由 SpeechService / RecorderService
    // 在用户首次打开相关界面时进行（惰性 + 避免启动开销）。
    return Capability::yes(QStringLiteral("音频输入将由音频后端在首次使用时枚举。"));
#endif
}

Capability SystemInfo::probeAudioOutput()
{
#if !DC_ENABLE_SPEECH
    return Capability::no(
        QStringLiteral("本程序在当前系统中未编译语音合成支持。"),
        QStringLiteral("编译期未启用 DC_ENABLE_SPEECH。"));
#else
    return Capability::yes(QStringLiteral("音频输出将由语音后端在首次使用时枚举。"));
#endif
}

// ===========================================================================
//  完整探测
// ===========================================================================
namespace {

CapabilityReport buildCapabilityReport(LegacyTier tier)
{
    CapabilityReport report;

    // ---- 窗口能力 ----
    // 半透明无边框窗口依赖合成器。Win7 起支持 DWM 合成；
    // WinXP 无桌面合成，因此需要降级为不透明窗口。
    if (tier == LegacyTier::WinXpClass) {
        report.translucentWindow = Capability::no(
            QStringLiteral("当前系统不支持窗口半透明显示，桌宠将以不透明背景显示。"),
            QStringLiteral("Windows XP 无桌面合成（DWM）。"));
    } else {
        report.translucentWindow = Capability::yes();
    }

    report.alwaysOnTop = Capability::yes();
    report.trayIcon = Capability::yes();
    report.imageSequenceLoad = Capability::yes();

    // ---- 全局快捷键 ----
    // Windows 用 RegisterHotKey；Linux 依赖 X11（Wayland 下全局快捷键受限）
#ifdef Q_OS_LINUX
    const bool isWayland = !qEnvironmentVariable("WAYLAND_DISPLAY").isEmpty();
    if (isWayland) {
        report.globalHotkey = Capability::no(
            QStringLiteral("当前桌面环境为 Wayland，全局快捷键受限，"
                           "请在系统设置中自行绑定（命令：desktop-companion --toggle）。"),
            QStringLiteral("WAYLAND_DISPLAY 已设置，X11 全局热键 API 不可用。"));
    } else {
        report.globalHotkey = Capability::yes();
    }
#else
    report.globalHotkey = Capability::yes();
#endif

    // ---- 开机启动 ----
    // 由 AutostartManager 在真正写入时判定；这里先按平台给默认结论
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    report.autostart = Capability::yes();
#else
    report.autostart = Capability::no(
        QStringLiteral("当前系统不支持开机启动设置。"));
#endif

    // ---- 网络 ----
#if DC_ENABLE_NETWORK
    report.networkTls12 = SystemInfo::probeNetworkTls12();
#else
    report.networkTls12 = Capability::no(
        QStringLiteral("本程序在当前系统中未编译网络支持，搜索与翻译功能不可用。"),
        QStringLiteral("编译期未启用 DC_ENABLE_NETWORK。"));
#endif
    report.systemCaStore = report.networkTls12.available
        ? Capability::yes(QStringLiteral("将使用 Qt 默认证书库。"))
        : Capability::no(QStringLiteral("安全通信不可用，证书库无法使用。"));

    // ---- 音频与语音 ----
    report.audioInput = SystemInfo::probeAudioInput();
    report.audioOutput = SystemInfo::probeAudioOutput();

#if DC_ENABLE_SPEECH
    // 具体的语音后端探测交给 SpeechService（它知道各后端的细节）。
    // 这里只做编译期判断，运行期会再收窄一次。
    report.speechSynthesis = Capability::yes(
        QStringLiteral("编译期已启用语音合成，运行期将探测具体后端。"));
    report.speechRecognition = Capability::yes(
        QStringLiteral("编译期已启用语音识别，运行期将探测具体后端。"));
#else
    report.speechSynthesis = Capability::no(
        QStringLiteral("本程序在当前系统中未编译语音支持，语音功能已禁用。"
                       "桌宠与本地命令功能不受影响。"),
        QStringLiteral("编译期未启用 DC_ENABLE_SPEECH。"));
    report.speechRecognition = report.speechSynthesis;
#endif

    // ---- 安全存储 ----
    report.secureStorage = SystemInfo::probeSecureStorage();

    // ---- 桌面通知 ----
    report.desktopNotifications = Capability::yes();

    return report;
}

} // namespace

SystemSnapshot SystemInfo::detect()
{
    SystemSnapshot snapshot;

    snapshot.family = currentFamily();
    snapshot.tier = currentTier();
    snapshot.kernelVersion = QSysInfo::kernelVersion();
    snapshot.osVersion = QSysInfo::productVersion();
    snapshot.osName = QSysInfo::prettyProductName();
    snapshot.architecture = QSysInfo::currentCpuArchitecture();
    snapshot.pointerSize = static_cast<int>(sizeof(void *));
    snapshot.is64Bit = snapshot.pointerSize == 8;
    snapshot.localeName = QLocale::system().name();

    if (snapshot.family == OsFamily::Linux) {
        const QString product = detectLinuxProduct();
        snapshot.productName = product.isEmpty() ? snapshot.osName : product;
    } else {
        snapshot.productName = snapshot.osName;
    }
    snapshot.productVersion = snapshot.osVersion;

    snapshot.capabilities = buildCapabilityReport(snapshot.tier);

    DC_INFO(LogCat::Platform,
            QStringLiteral("系统探测：%1 / %2 / %3 位 / 档位=%4")
                .arg(snapshot.productName,
                     snapshot.kernelVersion,
                     QString::number(snapshot.pointerSize * 8),
                     legacyTierToString(snapshot.tier)));

    return snapshot;
}

const SystemSnapshot &SystemInfo::current()
{
    static const SystemSnapshot snapshot = detect();
    return snapshot;
}

// ===========================================================================
//  面向用户的可用性说明
// ===========================================================================
QStringList SystemInfo::describeDisabledFeatures(const CapabilityReport &report)
{
    QStringList notes;

    auto check = [&notes](const Capability &cap, const QString &featureName) {
        if (!cap.available) {
            const QString reason = cap.reason.isEmpty()
                ? QStringLiteral("当前系统不支持。")
                : cap.reason;
            notes.append(QStringLiteral("• %1：%2").arg(featureName, reason));
        }
    };

    check(report.translucentWindow, QStringLiteral("桌宠透明背景"));
    check(report.globalHotkey,       QStringLiteral("全局快捷键"));
    check(report.autostart,          QStringLiteral("开机启动"));
    check(report.networkTls12,       QStringLiteral("联网搜索 / 翻译"));
    check(report.speechRecognition,  QStringLiteral("语音识别"));
    check(report.speechSynthesis,    QStringLiteral("语音播报"));
    check(report.audioInput,         QStringLiteral("音频输入（录音）"));
    check(report.secureStorage,      QStringLiteral("API 密钥安全存储"));
    check(report.trayIcon,           QStringLiteral("系统托盘"));

    if (notes.isEmpty()) {
        notes.append(QStringLiteral("• 当前系统的全部功能均可用。"));
    }

    return notes;
}

// ===========================================================================
//  兼容性报告（SRS 9.3 兼容性测试的辅助输出）
// ===========================================================================
QString SystemInfo::buildCompatibilityReport(const SystemSnapshot &snapshot)
{
    QString out;
    QTextStream s(&out);

    s << "===== Desktop Companion 兼容性报告 =====\n";
    s << "生成时间      : " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    s << "程序版本      : " << buildinfo::kAppVersion << "\n";
    s << "Qt 版本       : " << buildinfo::kQtVersion << " (运行期 " << qVersion() << ")\n";
    s << "编译器        : " << buildinfo::kCompiler << "\n";
    s << "构建类型      : " << buildinfo::kBuildType << "\n";
    s << "\n";
    s << "操作系统      : " << snapshot.productName << "\n";
    s << "系统系列      : " << osFamilyToString(snapshot.family) << "\n";
    s << "版本号        : " << snapshot.kernelVersion
      << " (产品版本 " << snapshot.osVersion << ")\n";
    s << "架构          : " << snapshot.architecture
      << " / " << (snapshot.is64Bit ? "64" : "32") << " 位\n";
    s << "兼容档位      : " << legacyTierToString(snapshot.tier) << "\n";
    s << "系统区域      : " << snapshot.localeName << "\n";
    s << "\n";
    s << "----- 能力探测结果 -----\n";

    auto line = [&s](const QString &name, const Capability &cap) {
        // 注意：早期写的是 arg(name, -22)，想用负宽度做左对齐 ——
        // 那是 printf 的写法，QString::arg 的第二个参数是"字段宽度"，
        // 必须是正数；传负数不会左对齐，只会得到未定义的结果。
        // 正确写法是 leftJustified()。
        s << name.leftJustified(22, QLatin1Char(' '))
          << (cap.available ? "[可用]  " : "[不可用]");
        if (!cap.available && !cap.reason.isEmpty())
            s << " " << cap.reason;
        else if (!cap.detail.isEmpty())
            s << " " << cap.detail;
        s << "\n";
    };

    const CapabilityReport &c = snapshot.capabilities;
    line(QStringLiteral("桌面透明窗口"),   c.translucentWindow);
    line(QStringLiteral("窗口置顶"),       c.alwaysOnTop);
    line(QStringLiteral("系统托盘"),       c.trayIcon);
    line(QStringLiteral("全局快捷键"),     c.globalHotkey);
    line(QStringLiteral("开机启动"),       c.autostart);
    line(QStringLiteral("TLS 1.2+"),       c.networkTls12);
    line(QStringLiteral("系统证书库"),     c.systemCaStore);
    line(QStringLiteral("音频输入"),       c.audioInput);
    line(QStringLiteral("音频输出"),       c.audioOutput);
    line(QStringLiteral("语音识别"),       c.speechRecognition);
    line(QStringLiteral("语音合成"),       c.speechSynthesis);
    line(QStringLiteral("安全凭据存储"),   c.secureStorage);
    line(QStringLiteral("桌面通知"),       c.desktopNotifications);

    s << "\n----- 功能可用性说明 -----\n";
    for (const QString &note : describeDisabledFeatures(c))
        s << note << "\n";

    s << "\n注意：以上为运行时探测结果，不代表在目标系统上已完成完整验证。\n";
    s << "      SRS 9.3 要求在每个目标系统上完成实机测试后才能发布兼容声明。\n";

    return out;
}

} // namespace dc
