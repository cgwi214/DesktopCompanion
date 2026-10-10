// ============================================================================
//  PlatformAdapter.h — 平台适配接口（唯一允许出现 OS 判断的抽象边界）
//
//  需求依据：SRS V1.1 3.2（模块依赖规则）
//    「平台差异由 PlatformAdapter 隔离，不在业务代码中散布操作系统判断。」
//
//  以及 6.2（平台适配要求）：
//    - 启动时检测操作系统、版本、架构和可用能力
//    - 以能力探测结果控制功能入口
//    - 避免依赖仅在新系统提供的 API；必要时提供条件编译或替代实现
//
//  设计说明：
//    业务层只持有 PlatformAdapter* 基类指针，从不 include 任何系统头文件。
//    具体实现：
//      PlatformAdapter_win.cpp    -> WindowsAdapter
//      PlatformAdapter_linux.cpp  -> LinuxAdapter
//    Windows/Linux 专有实现之间还隔了一层「通用回退实现」，
//    这样即使某个新系统没有专有实现，程序也能启动（弱化降级而非崩溃）。
// ============================================================================
#pragma once

#include "model/Types.h"
#include "platform/SystemInfo.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 打开目标的结果
// ---------------------------------------------------------------------------
struct OpenTargetResult {
    bool ok = false;
    QString errorCode;
    QString message;
    qint64 processId = 0;      // 启动的应用进程 pid（打开应用时有意义）
};

// ---------------------------------------------------------------------------
// PlatformAdapter —— 平台能力抽象
//
//  注意：这里的「打开应用 / 打开文件」仍会做参数校验，但权限判定与路径
//  白名单校验在上层（PermissionManager / SystemService）完成。
//  本层只负责「怎么在具体系统上做这件事」。
// ---------------------------------------------------------------------------
class PlatformAdapter : public QObject
{
    Q_OBJECT

public:
    explicit PlatformAdapter(QObject *parent = nullptr);
    ~PlatformAdapter() override;

    // 工厂：根据编译期平台返回对应实现；未支持平台返回通用回退实现
    static PlatformAdapter *create(QObject *parent = nullptr);

    // -----------------------------------------------------------------------
    // 单例访问
    //
    //  为什么平台层需要单例，而业务层不用：
    //    PlatformAdapter 是无状态的「怎么在具体系统上做这件事」的翻译层，
    //    它不持有业务状态，全局唯一既安全又省事。
    //    而 SystemService / PermissionManager 等业务模块一律用依赖注入
    //    （CoreRuntime 装配），这样才能在单元测试里替换掉它们。
    //
    //  返回值可能为 nullptr（当前平台无适配器）。调用方必须判空 ——
    //  这是刻意的：宁可让一个功能不可用，也不假装它能用。
    // -----------------------------------------------------------------------
    static PlatformAdapter *instance();

    // CoreRuntime 启动时调用一次，完成单例装配。
    // 若已有实例则先销毁旧的（便于测试中重复初始化）。
    static PlatformAdapter *createInstance(QObject *parent = nullptr);

    // 平台标识
    virtual QString platformId() const = 0;

    // -----------------------------------------------------------------------
    // 应用解析（SRS 4.3）
    // -----------------------------------------------------------------------

    // 在受信任的系统程序目录中按名称查找已安装程序，返回绝对路径。
    //
    //  重要：实现必须「不搜索当前工作目录」。
    //  Windows 上 CreateProcess / ShellExecute 的搜索顺序会把当前目录排在
    //  PATH 之前，攻击者只要在工作目录放一个同名 exe 就能劫持调用。
    //  因此这里只在系统目录 + PATH 中的绝对路径项里查找。
    //
    //  找不到返回空字符串（不是失败，是"没有这个程序"）。
    virtual QString resolveInstalledProgram(const QString &programName) const;

    // -----------------------------------------------------------------------
    // 应用与文件操作（SRS 4.3）
    // -----------------------------------------------------------------------

    // 启动程序。
    //   executablePath : 已经过校验的可执行文件绝对路径
    //   arguments      : 白名单校验后的参数列表（绝不做 shell 字符串拼接）
    //   workingDir     : 工作目录（可为空）
    // 实现要求：必须使用「参数数组 + 不经过 shell」的方式启动，
    //          这是防命令注入的关键（SRS 7.3）。
    virtual OpenTargetResult launchProcess(const QString &executablePath,
                                          const QStringList &arguments,
                                          const QString &workingDir = QString()) = 0;

    // 用系统默认程序打开文件/目录（与 launchProcess 不同，
    // 这里把决定权交给操作系统关联，因此对路径的校验必须更严格）
    virtual OpenTargetResult openWithDefaultApplication(const QString &absolutePath) = 0;

    // 在文件管理器中定位（选中）文件
    virtual OpenTargetResult revealInFileManager(const QString &absolutePath) = 0;

    // -----------------------------------------------------------------------
    // 路径与目录（SRS 4.3）
    // -----------------------------------------------------------------------
    virtual QString homeDirectory() const = 0;
    virtual QString desktopDirectory() const = 0;
    virtual QString documentsDirectory() const = 0;
    virtual QString downloadsDirectory() const = 0;

    // 便携模式判定：程序目录下是否存在标记文件（SRS 8.4）
    virtual bool isPortableMode() const;

    // -----------------------------------------------------------------------
    // 系统信息（SRS 6.1 / 6.2）
    // -----------------------------------------------------------------------
    virtual QString osName() const = 0;
    virtual QString osVersionString() const = 0;
    virtual bool isRunningAsAdmin() const = 0;

    // 当前进程是否已提升权限（用于向用户说明所需权限，SRS 4.4 第 3 条）
    virtual QString privilegeDescription() const;

    // -----------------------------------------------------------------------
    // 窗口与外观（SRS 4.1）
    // -----------------------------------------------------------------------
    // 是否支持无边框半透明窗口
    virtual bool supportsTranslucentWindow() const = 0;
    // 是否支持窗口置顶
    virtual bool supportsAlwaysOnTop() const = 0;
    // 把窗口设为「不抢占焦点」（桌宠不应打断用户当前工作，A.4.1 要求）
    virtual bool applyNoFocusPolicy(void *nativeWindowHandle) = 0;

    // -----------------------------------------------------------------------
    // 声音提示（可选，SRS 4.1 状态反馈）
    // -----------------------------------------------------------------------
    virtual bool beep() = 0;

    // -----------------------------------------------------------------------
    // 当前用户与系统信息（供安全审计与兼容性报告使用）
    // -----------------------------------------------------------------------
    virtual QString currentUserName() const;
    virtual QStringList systemApplicationHints() const;

signals:
    // 系统即将休眠/关机（用于安全停止录音，SRS 4.11）
    void systemIsShuttingDown();
};

} // namespace dc
