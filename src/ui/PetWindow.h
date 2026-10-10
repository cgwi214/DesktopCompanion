// ============================================================================
//  PetWindow.h — 桌宠主窗口
//
//  需求依据：SRS V1.1 4.1（PET-003 / PET-004 / PET-005 / PET-008）
//            4.2（TXT-003 快捷键唤起）、7.1（不阻塞 UI）
//
//  职责边界（刻意划得很窄）：
//    本类只负责【窗口行为】：
//      - 无边框、透明背景、可拖动（PET-003）
//      - 缩放（PET-004）
//      - 置顶开关
//      - 把"当前状态 + 当前帧"画出来（实际绘制委托给 PetRenderer）
//      - 承受双击/右键/拖拽等鼠标手势
//      - 驱动 PetStateMachine::tick()
//
//    它**不做**的事：
//      - 不解析命令（交给 IntentRouter）
//      - 不执行系统操作（交给 SystemService）
//      - 不知道有哪些服务存在
//
//  为什么这样划：
//    SRS 3.2 明确要求 "UI 层不得直接执行系统命令或访问网络"。
//    把窗口做薄，是让这条规则"写起来很难违规"的最有效办法 ——
//    窗口类里根本没有可用的服务指针。
//
//  拖动实现说明：
//    无边框窗口用 QMouseEvent 自己算偏移。
//    不用 QWindow::startSystemMove() 是因为在部分旧系统上不可用，
//    而 SRS 6.1 要求兼容旧版本（Win7 等）。
// ============================================================================
#pragma once

#include "model/Types.h"
#include "ui/PetRenderer.h"
#include "ui/PetStateMachine.h"

#include <QPoint>
#include <QRect>
#include <QWidget>

namespace dc {

class SpeechBubble;
class CommandInputBox;
class TrayController;

// ---------------------------------------------------------------------------
// 窗口配置（由 AppConfig::ui 映射）
// ---------------------------------------------------------------------------
struct PetWindowConfig {
    bool alwaysOnTop = true;
    double opacity = 1.0;         // 0.2 ~ 1.0
    double scale = 1.0;           // 0.5 ~ 3.0
    int posX = -1;                // -1 = 未设置，自动选位置
    int posY = -1;
    bool startHidden = false;
    bool showStatusHints = true;  // 是否在气泡里显示状态提示

    // 窗口是否需要接收鼠标（桌宠贴边时用户可能希望点击穿透）
    bool clickThrough = false;
};

// ---------------------------------------------------------------------------
// PetWindow
// ---------------------------------------------------------------------------
class PetWindow : public QWidget
{
    Q_OBJECT

public:
    explicit PetWindow(PetRenderer *renderer,
                       PetStateMachine *stateMachine,
                       QWidget *parent = nullptr);
    ~PetWindow() override;

    void setConfig(const PetWindowConfig &config);
    PetWindowConfig config() const;

    // 接入气泡窗口（用于显示消息与定位）
    void setBubble(SpeechBubble *bubble);

    // 当前窗口占用的屏幕区域（气泡靠它定位）
    QRect anchorRect() const;

    // -----------------------------------------------------------------------
    // 显示消息：转发给气泡，并同步驱动状态机
    //
    //  durationMs <= 0 表示不自动关闭
    // -----------------------------------------------------------------------
    void say(const QString &text, int durationMs = 4000);
    void sayWithChoices(const QString &prompt, const QStringList &options);
    void showProgress(const QString &text);
    void hideBubble();

    // 状态机代理（让外部不需要同时持有两个对象）
    PetStateMachine *stateMachine() const;

    // 立即刷新一帧（用于状态变化后立刻重绘，不等 tick）
    void refresh();

    // -----------------------------------------------------------------------
    // 屏幕边界保护：把窗口位置夹进可用区域（多显示器安全）
    //  公开以便单元测试
    // -----------------------------------------------------------------------
    static QPoint clampToScreen(const QPoint &desired, const QSize &windowSize);

    // 自动选一个初始位置（右下角偏内，避开任务栏）
    static QPoint defaultPosition(const QSize &windowSize);

public slots:
    // 从系统托盘/快捷键切换显示状态
    void toggleVisibility();
    void showPet();
    void hidePet();

    // 状态机状态变化 -> 重绘
    void onStateChanged(dc::PetState oldState, dc::PetState newState);
    void onFrameAdvanced(int frameIndex);

signals:
    // 用户双击桌宠（界面据此打开输入框）
    void activated();

    // 用户右键（界面据此弹菜单）
    void contextMenuRequested(const QPoint &globalPos);

    // 拖动结束（界面据此保存位置到配置）
    void positionChanged(const QPoint &newPos);

    // 请求打开/关闭输入框（由快捷键或双击触发）
    void inputRequested();
    void historyRequested();
    void settingsRequested();
    void quitRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    // 开始拖动
    void beginDrag(const QPoint &globalPos);
    // 拖动中
    void updateDrag(const QPoint &globalPos);
    // 结束拖动（必要时写回配置）
    void endDrag();

    // 按配置调整透明度
    void applyOpacity();

    // 计算窗口应有的尺寸（由渲染器决定）
    void applyRendererSize();

    PetRenderer *m_renderer = nullptr;
    PetStateMachine *m_stateMachine = nullptr;
    SpeechBubble *m_bubble = nullptr;

    PetWindowConfig m_config;

    // 拖动状态
    bool m_dragging = false;
    bool m_dragMoved = false;         // 是否真的移动过（区分点击与拖动）
    QPoint m_dragStartGlobal;
    QPoint m_dragStartWindowPos;
    QPoint m_lastEmittedPos;

    // 动画定时器
    int m_tickTimerId = 0;
    int m_tickIntervalMs = 200;

    // 悬停反馈
    bool m_hovered = false;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::PetWindowConfig)
