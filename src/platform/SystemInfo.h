// ============================================================================
//  SystemInfo.h — 操作系统检测与运行时能力探测
//
//  需求依据：SRS V1.1 第 6.2 节（平台适配要求）
//    - 启动时检测操作系统、版本、架构和可用能力
//    - 以能力探测结果控制功能入口，而不是仅凭系统名称判断
//    - 不支持的功能应禁用并解释原因
//    - 核心桌宠和本地规则命令不应因语音或网络模块缺失而无法启动
//    - 为旧系统建立独立构建配置和依赖版本清单
//
//  设计说明（为什么「不凭系统名判断」）：
//    同一个 Windows 7 可能是带 SP1 也可能不带；CentOS 6 上的
//    OpenSSL 版本差异会导致 TLS 1.2 是否可用完全不同。
//    因此这里只做「探测」，不下结论：
//      检测到 OS = Windows 7 并不会禁用网络，而是去实际探测
//      「本机 OpenSSL/系统证书是否支持 TLS 1.2」，据结果决定。
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 操作系统系列
// ---------------------------------------------------------------------------
enum class OsFamily {
    Unknown = 0,
    Windows,
    Linux,
    MacOS
};

QString osFamilyToString(OsFamily family);

// ---------------------------------------------------------------------------
// 旧系统档位（SRS 6.1 兼容策略表）
//   用于决定「是否需要启用兼容降级逻辑」，例如：
//     - 不支持无边框半透明 → 回退为不透明矩形窗口
//     - 不支持 tray 图标 → 隐藏托盘入口
//     - 不支持 tooltip 富文本 → 转为纯文本
// ---------------------------------------------------------------------------
enum class LegacyTier {
    Modern = 0,     // 新版 Windows / 现代 Linux 发行版
    Win7Class,      // Windows 7 / Windows 8（目标兼容）
    WinXpClass,     // Windows XP（目标兼容，需旧版工具链）
    LegacyLinux,    // CentOS 6 / RHEL 7 等
    Unknown
};

QString legacyTierToString(LegacyTier tier);

// ---------------------------------------------------------------------------
// 运行时能力探测结果
//
//  每一项能力都带 available + reason 两个字段。
//  reason 直接用于界面的「为什么这项是灰的」提示（SRS 6.2 要求解释原因）。
// ---------------------------------------------------------------------------
struct Capability {
    bool available = false;
    QString reason;         // 不可用原因（面向用户）
    QString detail;         // 技术细节（进日志）

    static Capability yes(const QString &detail = QString())
    {
        return Capability{ true, QString(), detail };
    }
    static Capability no(const QString &reason, const QString &detail = QString())
    {
        return Capability{ false, reason, detail };
    }
};

struct CapabilityReport {
    // --- 核心能力（缺失时不影响启动，但要降级表现）---
    Capability translucentWindow;   // 无边框半透明窗口
    Capability alwaysOnTop;         // 窗口置顶
    Capability trayIcon;            // 系统托盘
    Capability globalHotkey;        // 全局快捷键
    Capability autostart;           // 开机启动

    // --- 可选能力 ---
    Capability networkTls12;        // 是否可建立 TLS 1.2+
    Capability systemCaStore;       // 是否可访问系统证书库
    Capability audioInput;          // 音频输入（麦克风）
    Capability audioOutput;         // 音频输出
    Capability speechRecognition;   // 语音识别后端
    Capability speechSynthesis;     // 语音合成后端
    Capability secureStorage;       // 系统安全凭据存储
    Capability desktopNotifications;// 桌面通知
    Capability imageSequenceLoad;   // 序列帧动画加载（足够的内存与解码器）

    // 汇总可用性回答（供界面直接使用）
    bool anySpeechCapability() const
    {
        return speechRecognition.available || speechSynthesis.available;
    }
};

// ---------------------------------------------------------------------------
// 系统信息快照
// ---------------------------------------------------------------------------
struct SystemSnapshot {
    OsFamily family = OsFamily::Unknown;
    LegacyTier tier = LegacyTier::Unknown;

    QString osName;              // 如 "Windows 10"
    QString osVersion;           // 如 "10.0.19044"
    QString kernelVersion;
    QString architecture;        // x86_64 / i686 / aarch64 ...
    int pointerSize = 0;         // 4 或 8
    bool is64Bit = false;

    QString productName;         // Windows 产品名 / Linux 发行版名
    QString productVersion;
    QString localeName;

    CapabilityReport capabilities;
};

// ---------------------------------------------------------------------------
// SystemInfo —— 纯函数式探测，无状态
// ---------------------------------------------------------------------------
class SystemInfo
{
public:
    // 完整探测（启动时调用一次，结果缓存）
    static SystemSnapshot detect();

    // 缓存后的访问（避免重复探测开销，SRS 7.1）
    static const SystemSnapshot &current();

    // 单独探测某一项能力（惰性求值用）
    static Capability probeNetworkTls12();
    static Capability probeSecureStorage();
    static Capability probeAudioInput();
    static Capability probeAudioOutput();

    // ---- 平台判定小工具 ------------------------------------------------
    static OsFamily currentFamily();
    static LegacyTier currentTier();
    static bool isWindows();
    static bool isLinux();

    // 把探测结果整理成面向用户的「功能可用性说明」列表
    //   SRS 6.2：不支持的功能应禁用并解释原因
    static QStringList describeDisabledFeatures(const CapabilityReport &report);

    // 生成一份兼容性报告（写入日志/供用户导出，SRS 9.3）
    static QString buildCompatibilityReport(const SystemSnapshot &snapshot);
};

} // namespace dc
