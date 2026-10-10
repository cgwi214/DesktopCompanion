// ============================================================================
//  TrayController.h — 系统托盘图标与菜单
//
//  需求依据：SRS V1.1
//    4.1 PET-002：支持从托盘显示/隐藏桌宠
//    4.12：全局快捷键对应动作在托盘菜单里都要有等价入口
//    6.2：不支持的功能应禁用并解释原因
//
//  关键设计决定：
//
//    1. 【托盘不可用时，本类必须能"空着"存在】
//       某些精简系统 / 无桌面环境（SRS 6.1 提到 CentOS 6、RHEL 7 命令行
//       环境）没有托盘服务。此时 QSystemTrayIcon::isSystemTrayAvailable()
//       为 false。
//       程序绝不能因此崩溃或什么都不显示 —— 所以 TrayController 提供
//       isAvailable()，主界面据此决定"要不要依赖托盘才能退出"。
//
//    2. 【菜单项按能力动态禁用，并写明原因】
//       SRS 6.2 要求禁用要有解释。所以菜单项禁用时 setToolTip 里
//       写清楚为什么（例如"此系统不支持全局快捷键"），而不是干脆不显示。
//       不显示会让人以为程序没这个功能；显示但灰掉+说明才是对的做法。
//
//    3. 【本类不执行任何操作，只发信号】
//       和 PetWindow 一样，托盘菜单里每一项都只 emit 一个信号。
//       谁去响应（例如"打开设置"由主窗口处理）不是托盘该管的事。
//
//    4. 【图标不依赖外部文件】
//       托盘图标用代码画（QPixmap + QPainter）。
//       原因：托盘图标在程序最早/最晚阶段都可能要用，而此时资源目录
//       说不定还没就位；画出来的图标永远不会"加载失败变成白方块"。
//       另外用户自定义皮肤时也不应该把托盘图标改坏。
// ============================================================================
#pragma once

#include "model/Types.h"
#include "platform/SystemInfo.h"

#include <QObject>
#include <QPixmap>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QAction;
class QMenu;
class QSystemTrayIcon;
QT_END_NAMESPACE

namespace dc {

class TrayController;

// ---------------------------------------------------------------------------
// 托盘菜单项的能力开关
//
//  为什么单独做这个结构而不是直接读 CapabilityReport：
//    菜单项是否可用，除了"系统能力"之外还取决于"用户配置"
//    （例如用户在设置里关了麦克风，那"语音输入"就该灰掉）。
//    两者混在一起会让调用点很难写对，所以这里统一汇总。
// ---------------------------------------------------------------------------
struct TrayMenuState {
    bool trayAvailable = false;         // 系统托盘是否可用

    bool canTogglePet = true;
    bool canOpenInput = true;
    bool canOpenHistory = true;
    bool canOpenSettings = true;

    bool canUseVoice = false;           // 语音输入（需要识别后端 + 用户启用）
    bool canRecord = false;             // 录音（需要音频输入 + 用户启用）

    // 每一项不可用时的原因（SRS 6.2）
    QString voiceDisabledReason;
    QString recordDisabledReason;
    QString hotkeyDisabledReason;

    QString petVisibleText;             // "隐藏桌宠" / "显示桌宠"
    QString petImagePath;               // 用户自定义图标（可空）
    QString appName;
};

// ---------------------------------------------------------------------------
// TrayController
// ---------------------------------------------------------------------------
class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QObject *parent = nullptr);
    ~TrayController() override;

    // -----------------------------------------------------------------------
    // 初始化
    //
    //  会检查系统托盘可用性；不可用时返回 false，且之后所有
    //  show()/setMenuState() 都是安全的空操作（不会崩溃）。
    // -----------------------------------------------------------------------
    bool initialize(const TrayMenuState &state);

    bool isAvailable() const;
    QString unavailableReason() const;

    // 更新菜单项可用性与文案（配置变化 / 能力变化时调用）
    void setMenuState(const TrayMenuState &state);
    TrayMenuState menuState() const;

    // 显示 / 隐藏托盘图标
    void show();
    void hide();

    // -----------------------------------------------------------------------
    // 在指定位置弹出上下文菜单
    //
    //  桌宠右键时会调用它 —— 这样"托盘菜单"和"桌宠右键菜单"
    //  是同一个菜单对象，不可能出现两处菜单项不一致的情况。
    //  托盘不可用时不弹（此时桌宠右键会退化为打开设置）。
    // -----------------------------------------------------------------------
    void showContextMenu(const QPoint &globalPos);

    // -----------------------------------------------------------------------
    // 气泡通知（SRS 4.9：网络请求被拒等需要告知用户）
    //
    //  托盘通知只是补充渠道 —— 桌宠气泡才是主要渠道。
    //  系统不支持通知时静默跳过，不报错。
    // -----------------------------------------------------------------------
    void notify(const QString &title, const QString &message, bool warning = false);

    // -----------------------------------------------------------------------
    // 生成托盘图标（公开以便单元测试与"关于"页面复用）
    //
    //  size 为图标边长；state 决定表情（不同状态给不同表情，
    //  让用户瞟一眼托盘就知道程序在干嘛 —— 这也是状态提示的一部分）。
    // -----------------------------------------------------------------------
    static QPixmap buildTrayIcon(int size,
                                 PetState state = PetState::Idle,
                                 const QString &customImagePath = QString());

    // 是否有系统托盘（静态查询，不依赖实例）
    static bool isSystemTraySupported(QString *reason = nullptr);

signals:
    // 显示/隐藏桌宠
    void togglePetRequested();
    // 打开命令输入框
    void openInputRequested();
    // 打开历史记录
    void openHistoryRequested();
    // 打开设置
    void openSettingsRequested();
    // 开始/结束语音输入
    void voiceInputRequested();
    // 开始/结束录音
    void recordToggleRequested();
    // 重新加载命令规则（用户手改了 resources/commands/*.json 之后）
    void reloadRulesRequested();
    // 打开日志/数据目录（便于用户自助排查）
    void openDataDirectoryRequested();
    // 退出
    void quitRequested();

private:
    // 建菜单
    void buildMenu();
    // 按当前状态刷新所有 action 的 enabled / text / tooltip
    void applyMenuState();
    // 更新托盘图标的提示文字与图像
    void refreshIcon();

    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;

    QAction *m_togglePetAction = nullptr;
    QAction *m_openInputAction = nullptr;
    QAction *m_voiceInputAction = nullptr;
    QAction *m_recordAction = nullptr;
    QAction *m_openHistoryAction = nullptr;
    QAction *m_openSettingsAction = nullptr;
    QAction *m_reloadRulesAction = nullptr;
    QAction *m_openDataDirAction = nullptr;
    QAction *m_quitAction = nullptr;

    TrayMenuState m_state;

    bool m_available = false;
    QString m_unavailableReason;

    PetState m_currentPetState = PetState::Idle;
};

} // namespace dc
