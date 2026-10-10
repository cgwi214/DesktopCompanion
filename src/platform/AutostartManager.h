// ============================================================================
//  AutostartManager.h — 开机启动管理
//
//  需求依据：SRS V1.1 第 4.12 节（开机启动）
//    - Windows：优先使用当前用户启动文件夹或用户级启动项
//    - Linux：优先使用 XDG Autostart
//    - 不默认创建系统级服务或需要管理员权限的启动项
//    - 提供启动时最小化、启动后显示桌宠、延迟启动等选项
//    - 用户关闭开机启动后，应清理本程序创建的对应启动配置
//
//  安全约束（SRS 4.4）：
//    - 绝不写入需要管理员权限的位置
//    - 不创建 Windows 服务或计划任务
//    - 写入前校验目标位置，写入后记录结果
//    - 关闭时必须清理干净，不留残留项
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace dc {

class PlatformAdapter;

// ---------------------------------------------------------------------------
// 开机启动实现方式（SRS 4.12 的「优先」顺序）
// ---------------------------------------------------------------------------
enum class AutostartMethod {
    NotSupported = 0,
    WindowsStartupFolder,   // %APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup
    WindowsRegistryRun,     // HKCU\Software\Microsoft\Windows\CurrentVersion\Run
    XdgAutostart,           // ~/.config/autostart/*.desktop
    LaunchAgent             // macOS（本版本不实现，保留枚举）
};

QString autostartMethodToString(AutostartMethod method);

// ---------------------------------------------------------------------------
// 开机启动状态
// ---------------------------------------------------------------------------
struct AutostartStatus {
    bool enabled = false;
    bool methodAvailable = false;
    AutostartMethod method = AutostartMethod::NotSupported;
    QString location;          // 实际使用的文件/注册表位置（用于向用户展示）
    QString registeredCommand; // 已注册的命令行
    bool matchesCurrentExecutable = false;   // 已注册的是否就是当前程序
    QString unavailableReason; // 不可用原因（SRS 6.2）
};

// ---------------------------------------------------------------------------
// 开机启动参数
// ---------------------------------------------------------------------------
struct AutostartOptions {
    bool startMinimized = true;    // 启动时最小化（不显示主窗口）
    bool startHidden = false;      // 启动后完全隐藏桌宠（仅托盘）
    int delaySeconds = 0;          // 延迟启动秒数
};

// ---------------------------------------------------------------------------
// AutostartManager
// ---------------------------------------------------------------------------
class AutostartManager : public QObject
{
    Q_OBJECT

public:
    explicit AutostartManager(PlatformAdapter *adapter, QObject *parent = nullptr);
    ~AutostartManager() override;

    // 当前是否已启用（读取实际状态，而非配置值 —— 用户可能手工删除了启动项）
    AutostartStatus status() const;

    // 启用开机启动
    Result enable(const AutostartOptions &options);

    // 关闭开机启动，并清理本程序创建的启动配置（SRS 4.12 要求）
    Result disable();

    // 应用新的启动参数（已启用时更新启动项内容）
    Result update(const AutostartOptions &options);

    // 检测当前方法是否可用（SRS 6.2：不支持时禁用并解释原因）
    static bool isAvailable(AutostartMethod method, QString *reason = nullptr);

private:
    // 各平台实现
    Result enableWindowsStartupFolder(const AutostartOptions &options);
    Result enableWindowsRegistryRun(const AutostartOptions &options);
    Result enableXdgAutostart(const AutostartOptions &options);

    Result disableWindowsStartupFolder();
    Result disableWindowsRegistryRun();
    Result disableXdgAutostart();

    AutostartStatus statusWindowsStartupFolder() const;
    AutostartStatus statusWindowsRegistryRun() const;
    AutostartStatus statusXdgAutostart() const;

    // 选择实际使用的方法（按 SRS 4.12 的优先顺序）
    static AutostartMethod selectMethod();

    // 构造启动命令行（含参数）
    static QString buildCommandLine(const AutostartOptions &options);
    // 构造传给程序自身的参数列表
    static QStringList buildArguments(const AutostartOptions &options);

    // 清理操作产生的残留
    void cleanupStaleEntries();

    PlatformAdapter *m_adapter = nullptr;
};

} // namespace dc
