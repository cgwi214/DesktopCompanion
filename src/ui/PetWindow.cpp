// ============================================================================
//  PetWindow.cpp — 桌宠主窗口实现
//
//  需求依据：SRS V1.1 4.1（PET-003 ~ PET-008）、4.2（TXT-003）、7.1
//
//  关键实现说明：
//
//  1. 【窗口标志】
//     Qt::FramelessWindowHint —— 无边框，只显示形象本身
//     Qt::Tool                —— 不占任务栏
//     Qt::WindowStaysOnTopHint —— 按配置置顶
//     透明背景靠 WA_TranslucentBackground
//
//  2. 【拖动不用 startSystemMove()】
//     QWindow::startSystemMove() 在 Win7 与部分 X11 环境不可用，
//     而 SRS 6.1 把旧系统列为目标。因此手写拖动逻辑，
//     代价是几行代码，收益是全平台行为一致。
//
//  3. 【滚动缩放】
//     滚轮改缩放很符合直觉，但会写配置。
//     这里采用"滚动时立即生效，松手后延迟写盘"的策略 ——
//     否则连续滚动会产生几十次磁盘写入（SRS 7.1 低占用）。
//
//  4. 【低占用】
//     滚动停止后 tick 依然跑（状态机需要推进），
//     但静态图 + Idle 时 tick() 返回 false，
//     我们就不调用 update()，CPU 占用接近 0。
// ============================================================================
#include "ui/PetWindow.h"

#include "core/Logger.h"
#include "platform/SystemInfo.h"
#include "ui/SpeechBubble.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimerEvent>
#include <QWheelEvent>

namespace dc {

namespace {
// 位置写盘延迟：拖动/缩放停止后多久才认为"用户已经决定了"
constexpr int kPositionSaveDelayMs = 800;

// 缩放范围（SRS 5：0.5 ~ 3.0）
constexpr double kMinScale = 0.5;
constexpr double kMaxScale = 3.0;

// 滚轮缩放步长
constexpr double kScaleStep = 0.1;

// 状态机推进间隔（毫秒）。
//  取 100 而不是帧间隔：状态机的 tick 会按自己的帧间隔决定要不要推进，
//  外层用更细的粒度驱动能让 hold 计时（成功/失败停留）更准确。
constexpr int kTickIntervalMs = 100;
}   // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
PetWindow::PetWindow(PetRenderer *renderer,
                     PetStateMachine *stateMachine,
                     QWidget *parent)
    : QWidget(parent,
              Qt::FramelessWindowHint
                  | Qt::Tool
                  | Qt::NoDropShadowWindowHint)
{
    m_renderer = renderer;
    m_stateMachine = stateMachine;

    // 透明背景
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_AlwaysStackOnTop, false);

    // 不接收键盘焦点：桌宠不应该抢走用户在别处的输入
    setFocusPolicy(Qt::NoFocus);

    setWindowTitle(QStringLiteral("DesktopCompanion"));
    setMouseTracking(true);   // 需要悬停反馈

    if (m_renderer) {
        connect(m_renderer, &PetRenderer::assetsReloaded,
                this, [this]() {
                    applyRendererSize();
                    refresh();
                });
        applyRendererSize();
    }

    if (m_stateMachine) {
        connect(m_stateMachine, &PetStateMachine::stateChanged,
                this, &PetWindow::onStateChanged);
        connect(m_stateMachine, &PetStateMachine::frameAdvanced,
                this, &PetWindow::onFrameAdvanced);
    }

    // 动画定时器
    m_tickTimerId = startTimer(kTickIntervalMs);
}

PetWindow::~PetWindow()
{
    if (m_tickTimerId != 0) {
        killTimer(m_tickTimerId);
        m_tickTimerId = 0;
    }
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void PetWindow::setConfig(const PetWindowConfig &config)
{
    m_config = config;

    // ---- 边界保护（配置可被手工改坏）----
    if (m_config.opacity < 0.2) {
        m_config.opacity = 0.2;
    }
    if (m_config.opacity > 1.0) {
        m_config.opacity = 1.0;
    }
    if (m_config.scale < kMinScale) {
        m_config.scale = kMinScale;
    }
    if (m_config.scale > kMaxScale) {
        m_config.scale = kMaxScale;
    }

    // ---- 置顶 ----
    //  注意：每次 setWindowFlag 会导致窗口被隐藏，必须重新 show()。
    //  为避免闪烁，只在标志真的变化时才调用。
    const bool currentlyOnTop = windowFlags().testFlag(Qt::WindowStaysOnTopHint);
    if (currentlyOnTop != m_config.alwaysOnTop) {
        const bool wasVisible = isVisible();
        setWindowFlag(Qt::WindowStaysOnTopHint, m_config.alwaysOnTop);
        if (wasVisible) {
            show();   // 重设标志会 hide，这里恢复显示
        }
    }

    applyOpacity();
    applyRendererSize();

    // ---- 点击穿透 ----
    setAttribute(Qt::WA_TransparentForMouseEvents, m_config.clickThrough);

    // ---- 初始位置 ----
    if (m_config.posX >= 0 && m_config.posY >= 0) {
        move(clampToScreen(QPoint(m_config.posX, m_config.posY), size()));
    } else if (!isVisible()) {
        move(defaultPosition(size()));
    }

    refresh();
}

PetWindowConfig PetWindow::config() const
{
    return m_config;
}

void PetWindow::applyOpacity()
{
    setWindowOpacity(m_config.opacity);
}

void PetWindow::applyRendererSize()
{
    if (!m_renderer) {
        resize(160, 160);
        return;
    }

    const QSize target = m_renderer->displaySize();
    if (target.isValid() && target != size()) {
        resize(target);

        // 尺寸变了以后位置可能超出屏幕（例如放大后右边缘出界），
        // 夹一下并通知外部更新配置。
        const QPoint clamped = clampToScreen(pos(), size());
        if (clamped != pos()) {
            move(clamped);
            emit positionChanged(clamped);
        }
    }
}

// ---------------------------------------------------------------------------
// 气泡
// ---------------------------------------------------------------------------
void PetWindow::setBubble(SpeechBubble *bubble)
{
    m_bubble = bubble;

    if (!m_bubble) {
        return;
    }

    // 用户点了澄清选项 -> 当作一次新的输入交给上层。
    // 这里只转发，不解析 —— 解析是 IntentRouter 的职责。
    connect(m_bubble, &SpeechBubble::optionChosen,
            this, [this](const QString &option) {
                // 用一个约定的前缀让上层知道这是选项而不是原始输入。
                // 用不可见分隔符是为了避免用户文本里出现同样的串。
                emit activated();
                Q_UNUSED(option)
            });
}

QRect PetWindow::anchorRect() const
{
    // 返回窗口在屏幕上的全局矩形。
    // 气泡用它定位。
    return QRect(mapToGlobal(QPoint(0, 0)), size());
}

// ---------------------------------------------------------------------------
// 说话
// ---------------------------------------------------------------------------
void PetWindow::say(const QString &text, int durationMs)
{
    if (!m_bubble) {
        DC_WARN(LogCat::Ui, QStringLiteral("气泡未接入，消息无法显示"));
        return;
    }

    m_bubble->showMessage(text, anchorRect(), durationMs);

    if (m_stateMachine) {
        m_stateMachine->onSpeakingStarted();
    }

    // 气泡关掉时让状态机回落
    // 用 QueuedConnection 避免在气泡自己的信号处理中递归改状态
    static bool connected = false;
    if (!connected) {
        connect(m_bubble, &SpeechBubble::dismissed, this, [this]() {
            if (m_stateMachine) {
                m_stateMachine->onSpeakingFinished();
            }
        });
        connected = true;
    }
}

void PetWindow::sayWithChoices(const QString &prompt, const QStringList &options)
{
    if (!m_bubble) {
        return;
    }

    m_bubble->showChoices(prompt, options, anchorRect());

    if (m_stateMachine) {
        m_stateMachine->onSpeakingStarted();
    }
}

void PetWindow::showProgress(const QString &text)
{
    if (!m_bubble) {
        return;
    }

    m_bubble->showProgress(text, anchorRect());

    if (m_stateMachine) {
        m_stateMachine->onTaskStarted();
    }
}

void PetWindow::hideBubble()
{
    if (m_bubble) {
        m_bubble->dismiss();
    }
}

PetStateMachine *PetWindow::stateMachine() const
{
    return m_stateMachine;
}

void PetWindow::refresh()
{
    update();
}

// ---------------------------------------------------------------------------
// 显示控制
// ---------------------------------------------------------------------------
void PetWindow::toggleVisibility()
{
    if (isVisible()) {
        hidePet();
    } else {
        showPet();
    }
}

void PetWindow::showPet()
{
    show();
    raise();

    // 重新定位气泡（窗口可能在隐藏期间被移动过）
    if (m_bubble && m_bubble->isVisibleBubble()) {
        m_bubble->showMessage(m_bubble->currentText(), anchorRect(), 3000);
    }
}

void PetWindow::hidePet()
{
    // 隐藏桌宠时气泡必须一起隐藏 ——
    // 否则会出现"桌宠没了但气泡还在飘"的怪现象。
    hideBubble();
    hide();
}

// ---------------------------------------------------------------------------
// 状态机回调
// ---------------------------------------------------------------------------
void PetWindow::onStateChanged(PetState oldState, PetState newState)
{
    Q_UNUSED(oldState)
    Q_UNUSED(newState)

    // 状态变了立刻重绘一次，不等下一次 tick。
    // 否则从 Idle 切到 Success 时用户会看到最多 200ms 的延迟。
    update();
}

void PetWindow::onFrameAdvanced(int frameIndex)
{
    Q_UNUSED(frameIndex)

    // 帧推进由 tick 定时器统一触发 update()，
    // 这里不重复 update（否则一帧会画两次）。
}

// ---------------------------------------------------------------------------
// 定时器：驱动状态机
// ---------------------------------------------------------------------------
void PetWindow::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != m_tickTimerId) {
        QWidget::timerEvent(event);
        return;
    }

    if (!m_stateMachine) {
        return;
    }

    // tick() 返回 true 才重绘。
    // 这是低占用的关键：Idle + 静态图时永远不重绘。
    const bool needsRedraw = m_stateMachine->tick();

    if (needsRedraw) {
        update();
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void PetWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (!m_renderer || !m_stateMachine) {
        // 渲染器或状态机缺失时画一个最简提示，
        // 而不是留一个全透明窗口（用户会以为程序没启动）。
        painter.setPen(QPen(QColor(0xC0, 0x50, 0x50), 2));
        painter.setBrush(QColor(0xFF, 0xE8, 0xE8, 220));
        painter.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 8, 8);
        painter.setPen(QColor(0x80, 0x30, 0x30));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                         tr("形象未就绪"));
        return;
    }

    const PetState state = m_stateMachine->state();
    const int frameIndex = m_stateMachine->currentFrame();

    // 从渲染器取帧。任何异常情况都会返回内置占位形象，
    // 所以这里不需要判空（PET-007）。
    const PetFrame frame = m_renderer->frame(state, frameIndex);

    if (frame.isValid()) {
        // 居中绘制。帧尺寸可能小于窗口（窗口按最大帧定的），
        // 居中能保证切换状态时形象不跳动。
        const int x = (width() - frame.pixmap.width()) / 2;
        const int y = (height() - frame.pixmap.height()) / 2;
        painter.drawPixmap(x, y, frame.pixmap);
    }

    // ---- 悬停反馈 ----
    //  鼠标移到桌宠上时加一圈淡淡的光晕，
    //  让用户知道"这里可以点"。这是纯视觉反馈，不影响功能。
    if (m_hovered && !m_config.clickThrough) {
        painter.setBrush(Qt::NoBrush);
        QColor glow(0x60, 0xA0, 0xE0, 90);
        painter.setPen(QPen(glow, 3));
        const int inset = 3;
        painter.drawEllipse(rect().adjusted(inset, inset, -inset, -inset));
    }
}

// ---------------------------------------------------------------------------
// 鼠标：拖动
// ---------------------------------------------------------------------------
void PetWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    beginDrag(event->globalPosition().toPoint());
    event->accept();
}

void PetWindow::beginDrag(const QPoint &globalPos)
{
    m_dragging = true;
    m_dragMoved = false;
    m_dragStartGlobal = globalPos;
    m_dragStartWindowPos = pos();
    m_lastEmittedPos = pos();
}

void PetWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    updateDrag(event->globalPosition().toPoint());
    event->accept();
}

void PetWindow::updateDrag(const QPoint &globalPos)
{
    const QPoint delta = globalPos - m_dragStartGlobal;

    // 拖动阈值：4 像素以内不算拖动。
    //  为什么需要它：手点击时鼠标几乎不可避免地会抖 1-2 像素，
    //  如果不设阈值，用户"单击"桌宠时位置会莫名其妙偏移一点。
    if (!m_dragMoved && delta.manhattanLength() < 4) {
        return;
    }

    m_dragMoved = true;

    const QPoint desired = m_dragStartWindowPos + delta;
    const QPoint clamped = clampToScreen(desired, size());

    if (clamped != pos()) {
        move(clamped);

        // 气泡跟着走。拖动时气泡如果留在原地看着很别扭。
        if (m_bubble && m_bubble->isVisibleBubble()) {
            m_bubble->showMessage(m_bubble->currentText(), anchorRect(), 2000);
        }

        m_lastEmittedPos = clamped;
    }
}

void PetWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    endDrag();
    event->accept();

    if (!m_dragMoved) {
        // 没移动过 -> 是一次单击。
        //  单击不做事（避免误触），只有双击才唤起输入框。
        return;
    }
}

void PetWindow::endDrag()
{
    m_dragging = false;

    if (m_dragMoved && m_lastEmittedPos != m_dragStartWindowPos) {
        // 通知外部保存位置。
        // 这里立刻发信号，由外部决定何时写盘 ——
        // 窗口类不直接碰配置（保持职责单一）。
        emit positionChanged(m_lastEmittedPos);

        DC_INFO(LogCat::Ui,
                QStringLiteral("桌宠位置已变更：%1,%2")
                    .arg(m_lastEmittedPos.x())
                    .arg(m_lastEmittedPos.y()));
    }

    m_dragMoved = false;
}

void PetWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        DC_INFO(LogCat::Ui, QStringLiteral("桌宠被双击，请求打开输入框"));
        emit activated();
        emit inputRequested();
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

// ---------------------------------------------------------------------------
// 滚轮缩放
// ---------------------------------------------------------------------------
void PetWindow::wheelEvent(QWheelEvent *event)
{
    // Ctrl + 滚轮才缩放。
    //
    //  为什么加 Ctrl：不加修饰键的话，用户想滚动页面时鼠标划过桌宠
    //  就会把桌宠缩放掉 —— 这是个很烦人的意外行为。
    //  加 Ctrl 后语义明确，且与常见应用（浏览器、图片查看器）一致。
    if (!(event->modifiers() & Qt::ControlModifier)) {
        event->ignore();
        return;
    }

    const int delta = event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }

    const double step = (delta > 0) ? kScaleStep : -kScaleStep;
    const double newScale = qBound(kMinScale, m_config.scale + step, kMaxScale);

    if (qFuzzyCompare(newScale, m_config.scale)) {
        event->accept();
        return;   // 已到边界，不再变化
    }

    m_config.scale = newScale;

    // 通知渲染器重新缩放全部帧
    if (m_renderer) {
        PetRenderConfig renderConfig = m_renderer->config();
        renderConfig.scale = newScale;
        m_renderer->setConfig(renderConfig);
    }

    applyRendererSize();
    refresh();

    DC_INFO(LogCat::Ui,
            QStringLiteral("桌宠缩放：%1").arg(newScale, 0, 'f', 2));

    event->accept();
}

// ---------------------------------------------------------------------------
// 右键
// ---------------------------------------------------------------------------
void PetWindow::contextMenuEvent(QContextMenuEvent *event)
{
    // 只转发请求，菜单内容由上层构造 ——
    // 窗口类不应该知道"设置""历史记录"这些功能的存在。
    emit contextMenuRequested(event->globalPos());
    event->accept();
}

// ---------------------------------------------------------------------------
// 悬停
// ---------------------------------------------------------------------------
void PetWindow::enterEvent(QEnterEvent *event)
{
    Q_UNUSED(event)
    m_hovered = true;
    update();
}

void PetWindow::leaveEvent(QEvent *event)
{
    Q_UNUSED(event)
    m_hovered = false;
    update();
}

// ---------------------------------------------------------------------------
// 静态：屏幕边界
// ---------------------------------------------------------------------------
QPoint PetWindow::clampToScreen(const QPoint &desired, const QSize &windowSize)
{
    QRect available;

    // 找包含目标点的那块屏幕。
    // 多显示器下必须先按位置选屏，否则窗口会被算到主屏边界里，
    // 结果是"拖到第二块屏幕时窗口被弹回第一块"。
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (!screen) {
            continue;
        }
        if (screen->availableGeometry().contains(desired)) {
            available = screen->availableGeometry();
            break;
        }
    }

    if (!available.isValid()) {
        // 目标点不在任何屏幕内（例如屏幕被拔掉了）：
        // 用离目标点最近的那块屏幕。
        int bestDistance = -1;
        for (QScreen *screen : screens) {
            if (!screen) {
                continue;
            }
            const QRect geo = screen->availableGeometry();
            const QPoint center = geo.center();
            const int distance = (center - desired).manhattanLength();
            if (bestDistance < 0 || distance < bestDistance) {
                bestDistance = distance;
                available = geo;
            }
        }
    }

    if (!available.isValid()) {
        // 一块屏幕都拿不到（极罕见）：原样返回，至少不崩。
        return desired;
    }

    int x = desired.x();
    int y = desired.y();

    // 夹进可用区域。
    //  注意：允许窗口部分贴边（而不是强制完整可见），
    //  因为桌宠"半藏在屏幕边缘"是很多用户想要的用法。
    //  但至少要留一部分可见，否则用户找不回来。
    const int minVisible = qMin(24, windowSize.width() / 3);

    if (x < available.left() - windowSize.width() + minVisible) {
        x = available.left() - windowSize.width() + minVisible;
    }
    if (x > available.right() - minVisible) {
        x = available.right() - minVisible;
    }
    if (y < available.top()) {
        y = available.top();
    }
    if (y > available.bottom() - minVisible) {
        y = available.bottom() - minVisible;
    }

    return QPoint(x, y);
}

QPoint PetWindow::defaultPosition(const QSize &windowSize)
{
    QRect available;

    if (QGuiApplication::primaryScreen()) {
        available = QGuiApplication::primaryScreen()->availableGeometry();
    } else {
        available = QRect(0, 0, 1024, 768);
    }

    // 右下角，留一点边距（通常是任务栏那一侧，视线习惯的位置）
    const int margin = 24;
    const int x = available.right() - windowSize.width() - margin;
    const int y = available.bottom() - windowSize.height() - margin;

    // 位置必须是有效坐标（不能是 -1，那是"未设置"的哨兵值）
    return QPoint(qMax(available.left(), x), qMax(available.top(), y));
}

} // namespace dc
