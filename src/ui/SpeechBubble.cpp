// ============================================================================
//  SpeechBubble.cpp — 对话气泡实现
//
//  需求依据：SRS V1.1 4.2（TXT-002 / TXT-005）、4.6（澄清）、7.3（输入校验）
//
//  实现要点：
//    1. 独立无边框窗口，不抢焦点（Qt::WindowDoesNotAcceptFocus）
//    2. 位置自动避开屏幕边缘（computePosition 可单测）
//    3. 文本长度上限保护（SRS 7.3：所有外部输入均需校验）
//    4. 悬停暂停倒计时（长文本读得完）
//    5. 澄清选项不自动关闭（不替用户做决定）
// ============================================================================
#include "ui/SpeechBubble.h"

#include "core/Logger.h"
#include "ui/I18n.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimerEvent>

namespace dc {

namespace {
// 文本长度上限。
//  为什么是 2000：气泡是提示，不是文档阅读器。
//  超过 2000 字的回复应该引导用户去看历史记录，
//  而不是让一个气泡撑满屏幕（TXT-005：不遮挡关键区域）。
constexpr int kMaxTextLength = 2000;

// 最多几个选项。太多选项应该改用别的交互方式（例如打开历史/设置）。
constexpr int kMaxOptions = 5;

// 单条选项的文本上限
constexpr int kMaxOptionLength = 60;

// 进度动画的帧间隔（毫秒）
constexpr int kProgressIntervalMs = 120;

// 位置微调：气泡与桌宠之间的间隙
constexpr int kAnchorGap = 6;
}   // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
SpeechBubble::SpeechBubble(QWidget *parent)
    : QWidget(parent,
              // 关键标志位说明：
              //   Qt::Tool              —— 不在任务栏显示（气泡不该占任务栏）
              //   Qt::FramelessWindowHint —— 无边框，自己画圆角
              //   Qt::WindowDoesNotAcceptFocus —— 不抢输入焦点
              //   Qt::NoDropShadowWindowHint —— 阴影自己画，避免系统阴影不一致
              Qt::Tool
                  | Qt::FramelessWindowHint
                  | Qt::WindowDoesNotAcceptFocus
                  | Qt::NoDropShadowWindowHint
                  | Qt::WindowStaysOnTopHint)
{
    // 透明背景，圆角与阴影全部自绘
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, false);

    // 默认不接收鼠标（点击穿透）。
    // 有选项时由 showChoices 打开。
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    m_style.font = font();
    m_style.font.setPointSizeF(qMax(9.0, font().pointSizeF()));

    hide();
}

SpeechBubble::~SpeechBubble()
{
    if (m_timerId != 0) {
        killTimer(m_timerId);
    }
    if (m_animationTimerId != 0) {
        killTimer(m_animationTimerId);
    }
}

// ---------------------------------------------------------------------------
// 样式
// ---------------------------------------------------------------------------
void SpeechBubble::setStyle(const BubbleStyle &style)
{
    m_style = style;
    if (m_style.maxWidth < 120) {
        m_style.maxWidth = 120;   // 下限保护：太窄没法看
    }
    if (isVisible()) {
        relayout();
        update();
    }
}

BubbleStyle SpeechBubble::style() const
{
    return m_style;
}

// ---------------------------------------------------------------------------
// 显示消息
// ---------------------------------------------------------------------------
void SpeechBubble::showMessage(const QString &text,
                               const QRect &anchor,
                               int durationMs,
                               bool autoCloseOnHover)
{
    // ---- 输入校验（SRS 7.3）----
    //  控制字符会让绘制出问题（换行符要保留，其余去掉）。
    QString sanitized;
    sanitized.reserve(text.size());

    for (const QChar &ch : text) {
        if (ch == QLatin1Char('\n') || ch == QLatin1Char('\t')) {
            sanitized.append(ch == QLatin1Char('\t') ? QStringLiteral("    ") : ch);
        } else if (ch.isPrint() || ch.isSpace()) {
            sanitized.append(ch);
        }
        // 其余（控制字符、零宽字符）丢弃
    }

    if (sanitized.trimmed().isEmpty()) {
        // 空消息不弹空气泡 —— 那只会闪一下让用户困惑
        return;
    }

    if (sanitized.size() > kMaxTextLength) {
        sanitized.truncate(kMaxTextLength);
        sanitized.append(QStringLiteral("…"));
        DC_WARN(LogCat::Ui,
                QStringLiteral("气泡文本超过 %1 字，已截断").arg(kMaxTextLength));
    }

    m_text = sanitized;
    m_options.clear();
    m_showOptions = false;
    m_isProgress = false;
    m_optionRects.clear();
    m_hoveredOption = -1;

    m_anchor = anchor;
    m_durationMs = durationMs;
    m_autoCloseOnHover = autoCloseOnHover;

    // 鼠标穿透（无选项的气泡）
    setAttribute(Qt::WA_TransparentForMouseEvents, true);

    // 停掉进度动画
    if (m_animationTimerId != 0) {
        killTimer(m_animationTimerId);
        m_animationTimerId = 0;
    }

    relayout();

    // ---- 倒计时 ----
    if (m_timerId != 0) {
        killTimer(m_timerId);
        m_timerId = 0;
    }
    m_elapsedMs = 0;
    m_hovered = false;

    if (m_durationMs > 0) {
        // 用 50ms 一跳而不是 1000ms：
        // 这样悬停暂停的响应足够灵敏，且计时精度不依赖系统定时器精度。
        m_timerId = startTimer(50);
    }

    show();
    raise();

    DC_INFO(LogCat::Ui,
            QStringLiteral("气泡显示（%1 字，%2 ms）")
                .arg(m_text.size())
                .arg(m_durationMs > 0 ? QString::number(m_durationMs)
                                      : QStringLiteral("不自动关闭")));
}

// ---------------------------------------------------------------------------
// 显示选项
// ---------------------------------------------------------------------------
void SpeechBubble::showChoices(const QString &prompt,
                               const QStringList &options,
                               const QRect &anchor)
{
    if (options.isEmpty()) {
        // 没有选项就退化成普通消息，避免显示一个空的选项区
        showMessage(prompt, anchor, 8000);
        return;
    }

    QStringList sanitizedOptions;
    for (const QString &option : options) {
        QString clean = option.trimmed();
        if (clean.isEmpty()) {
            continue;
        }
        if (clean.size() > kMaxOptionLength) {
            clean.truncate(kMaxOptionLength);
            clean.append(QStringLiteral("…"));
        }
        sanitizedOptions << clean;
        if (sanitizedOptions.size() >= kMaxOptions) {
            break;
        }
    }

    if (sanitizedOptions.isEmpty()) {
        showMessage(prompt, anchor, 8000);
        return;
    }

    showMessage(prompt, anchor, 0);   // 0 = 不自动关闭
    m_options = sanitizedOptions;
    m_showOptions = true;

    // 有选项时必须能接收点击 —— 否则用户点了等于点桌面，非常困惑
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    relayout();
    update();
}

// ---------------------------------------------------------------------------
// 进度提示
// ---------------------------------------------------------------------------
void SpeechBubble::showProgress(const QString &text, const QRect &anchor)
{
    showMessage(text, anchor, 0);   // 不自动关闭，由调用方在结束时关闭
    m_isProgress = true;

    if (m_animationTimerId == 0) {
        m_animationTimerId = startTimer(kProgressIntervalMs);
    }

    update();
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------
void SpeechBubble::dismiss()
{
    if (!isVisible()) {
        return;
    }
    dismissImmediately();
}

void SpeechBubble::dismissImmediately()
{
    if (m_timerId != 0) {
        killTimer(m_timerId);
        m_timerId = 0;
    }
    if (m_animationTimerId != 0) {
        killTimer(m_animationTimerId);
        m_animationTimerId = 0;
    }
    m_elapsedMs = 0;
    m_hovered = false;
    m_hoveredOption = -1;

    if (isVisible()) {
        hide();
        emit dismissed();
    }
}

bool SpeechBubble::isVisibleBubble() const
{
    return isVisible();
}

QString SpeechBubble::currentText() const
{
    return m_text;
}

// ---------------------------------------------------------------------------
// 定时器
// ---------------------------------------------------------------------------
void SpeechBubble::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_timerId) {
        // 悬停暂停：鼠标在气泡上时不推进计时。
        // 但我们用 WA_TransparentForMouseEvents 让鼠标穿透了，
        // 所以普通气泡永远收不到 enter/leave 事件。
        // 因此这里只在**有选项**（可交互）时才真正暂停。
        if (m_autoCloseOnHover && m_hovered && !m_options.isEmpty()) {
            return;
        }

        m_elapsedMs += 50;

        if (m_durationMs > 0 && m_elapsedMs >= m_durationMs) {
            DC_INFO(LogCat::Ui, QStringLiteral("气泡超时自动关闭"));
            dismissImmediately();
        }
        return;
    }

    if (event->timerId() == m_animationTimerId) {
        // 进度指示的相位推进（0..7 循环，用于画小圆点）
        m_progressPhase = (m_progressPhase + 1) % 8;
        update();
        return;
    }

    QWidget::timerEvent(event);
}

// ---------------------------------------------------------------------------
// 尺寸计算
// ---------------------------------------------------------------------------
QSize SpeechBubble::computeContentSize() const
{
    QFontMetrics metrics(m_style.font);

    // 文本可用的最大宽度：总宽上限减去内边距
    const int textMaxWidth = qMax(40, m_style.maxWidth - m_style.paddingHorizontal * 2);

    // 用 QFontMetrics::boundingRect 配合 TextWordWrap 得到换行后的实际尺寸
    const QRect textRect = metrics.boundingRect(
        QRect(0, 0, textMaxWidth, 10000),
        Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
        m_text.isEmpty() ? QStringLiteral(" ") : m_text);

    int width = textRect.width();
    int height = textRect.height();

    // 加上行间距（QFontMetrics 不包含我们的自定义 lineSpacing）
    const int lineCount = qMax(1, metrics.lineSpacing() > 0
                                      ? (height / metrics.lineSpacing())
                                      : 1);
    height += (lineCount - 1) * m_style.lineSpacing;

    // 选项区宽度可能比文本更宽
    if (m_showOptions && !m_options.isEmpty()) {
        for (const QString &option : m_options) {
            const int optionWidth =
                metrics.horizontalAdvance(option) + m_style.paddingHorizontal;
            width = qMax(width, optionWidth);
        }
    }

    width = qBound(60, width, m_style.maxWidth - m_style.paddingHorizontal * 2);

    return QSize(width, height);
}

int SpeechBubble::optionsHeight() const
{
    if (!m_showOptions || m_options.isEmpty()) {
        return 0;
    }

    QFontMetrics metrics(m_style.font);
    const int rowHeight = metrics.height() + 8;      // 8 = 选项内上下留白
    const int gap = 4;

    // 提示文本与选项之间留一点间隔
    return 8 + m_options.size() * (rowHeight + gap);
}

QSize SpeechBubble::totalSize() const
{
    const QSize content = computeContentSize();

    const int width = content.width() + m_style.paddingHorizontal * 2
                      + m_style.borderWidth * 2 + 2;   // +2 给抗锯齿留余量

    const int height = content.height()
                       + optionsHeight()
                       + m_style.paddingVertical * 2
                       + m_style.tailHeight
                       + m_style.borderWidth * 2 + 2;

    return QSize(qMax(1, width), qMax(1, height));
}

// ---------------------------------------------------------------------------
// 定位
//
//  这是本类最需要单测的部分（边界处理容易错）：
//    1. 优先在 anchor 上方
//    2. 上方空间不足 -> 放下方
//    3. 左右夹在可用区域内
//    4. 上下都放不下 -> 放下方并尽量贴边（宁可靠边也不要超出屏幕）
// ---------------------------------------------------------------------------
QPoint SpeechBubble::computePosition(const QRect &anchor,
                                     const QSize &bubbleSize,
                                     const QRect &availableScreen) const
{
    const int tailWidthHalf = m_style.tailWidth / 2;

    // ---- 水平：与 anchor 居中对齐 ----
    int x = anchor.center().x() - bubbleSize.width() / 2;

    // 夹在屏幕内
    const int minX = availableScreen.left();
    const int maxX = availableScreen.right() - bubbleSize.width() + 1;
    if (x < minX) {
        x = minX;
    }
    if (maxX >= minX && x > maxX) {
        x = maxX;
    }

    // 尾巴位置由绘制时按 anchor 中心重新计算，
    // 这里只需保证窗口本身在屏幕内。

    // ---- 垂直：优先上方 ----
    const int spaceAbove = anchor.top() - availableScreen.top();
    const int neededAbove = bubbleSize.height() + kAnchorGap;

    int y = 0;
    if (spaceAbove >= neededAbove) {
        // 上方放得下：气泡底边贴着 anchor 顶部
        y = anchor.top() - kAnchorGap - bubbleSize.height();
    } else {
        // 上方放不下：放到下方
        y = anchor.bottom() + kAnchorGap;

        // 下方也放不下（桌宠几乎占满屏幕高度）：
        // 退而求其次，贴住屏幕下边缘附近可用的位置。
        const int spaceBelow = availableScreen.bottom() - anchor.bottom();
        if (spaceBelow < bubbleSize.height() + kAnchorGap) {
            y = availableScreen.bottom() - bubbleSize.height() + 1;
            if (y < availableScreen.top()) {
                y = availableScreen.top();
            }
        }
    }

    // 兜底：任何情况下不超出屏幕
    if (y < availableScreen.top()) {
        y = availableScreen.top();
    }
    const int maxY = availableScreen.bottom() - bubbleSize.height() + 1;
    if (maxY >= availableScreen.top() && y > maxY) {
        y = maxY;
    }

    Q_UNUSED(tailWidthHalf)

    return QPoint(x, y);
}

void SpeechBubble::relayout()
{
    const QSize size = totalSize();
    resize(size);

    // 找 anchor 所在的屏幕。
    // 多显示器时必须用 anchor 所在那块屏幕的区域，
    // 否则气泡可能被算到主屏边界里而跑到别的屏幕上。
    m_availableScreen = QRect(QPoint(0, 0), QSize(800, 600));   // 保守默认值

    if (QGuiApplication::primaryScreen()) {
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (QScreen *screen : screens) {
            if (!screen) {
                continue;
            }
            if (screen->availableGeometry().intersects(m_anchor)) {
                m_availableScreen = screen->availableGeometry();
                break;
            }
        }
        if (m_availableScreen.size() == QSize(800, 600)) {
            // 没找到相交的屏幕（anchor 可能是空矩形），退回主屏
            m_availableScreen = QGuiApplication::primaryScreen()->availableGeometry();
        }
    }

    const QPoint pos = computePosition(m_anchor, size, m_availableScreen);
    move(pos);

    layoutOptionRects();
}

// ---------------------------------------------------------------------------
// 选项布局与命中测试
// ---------------------------------------------------------------------------
void SpeechBubble::layoutOptionRects()
{
    m_optionRects.clear();

    if (!m_showOptions || m_options.isEmpty()) {
        return;
    }

    QFontMetrics metrics(m_style.font);
    const int rowHeight = metrics.height() + 8;
    const int gap = 4;

    // 内容区起点
    const int contentLeft = m_style.borderWidth + m_style.paddingHorizontal;
    const int contentWidth = width() - contentLeft * 2;

    // 选项区顶部：文本内容之下
    const QSize content = computeContentSize();
    int y = m_style.borderWidth + m_style.paddingVertical + content.height() + 8;

    for (int i = 0; i < m_options.size(); ++i) {
        m_optionRects << QRect(contentLeft, y, contentWidth, rowHeight);
        y += rowHeight + gap;
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void SpeechBubble::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // ---- 气泡主体矩形（扣掉尾巴高度）----
    const int bodyHeight = height() - m_style.tailHeight;
    QRectF bodyRect(m_style.borderWidth,
                    m_style.borderWidth,
                    width() - m_style.borderWidth * 2,
                    bodyHeight - m_style.borderWidth * 2);

    // ---- 尾巴（指向桌宠的小三角）----
    //  尾巴的水平位置对准 anchor 中心，但要夹在气泡宽度内，
    //  否则桌宠在屏幕边缘时尾巴会跑到气泡外面。
    const int tailCenterX = qBound(m_style.cornerRadius + m_style.tailWidth,
                                  m_anchor.center().x() - x(),
                                  width() - m_style.cornerRadius - m_style.tailWidth);

    // 判断气泡在桌宠上方还是下方，决定尾巴朝下还是朝上
    const bool bubbleAbove = (y() + height() <= m_anchor.top() + 2);

    QPainterPath bubblePath;
    bubblePath.addRoundedRect(bodyRect, m_style.cornerRadius, m_style.cornerRadius);

    // 尾巴三角
    QPolygonF tail;
    if (bubbleAbove) {
        // 气泡在上方 -> 尾巴朝下
        tail << QPointF(tailCenterX - m_style.tailWidth / 2.0, bodyRect.bottom() - 1)
             << QPointF(tailCenterX , bodyRect.bottom() + m_style.tailHeight)
             << QPointF(tailCenterX + m_style.tailWidth / 2.0, bodyRect.bottom() - 1);
    } else {
        // 气泡在下方 -> 尾巴朝上
        tail << QPointF(tailCenterX - m_style.tailWidth / 2.0, bodyRect.top() + 1)
             << QPointF(tailCenterX, bodyRect.top() - m_style.tailHeight)
             << QPointF(tailCenterX + m_style.tailWidth / 2.0, bodyRect.top() + 1);
    }

    // 把尾巴并入路径，这样填充与描边是连续的（不会有接缝）
    QPainterPath tailPath;
    tailPath.addPolygon(tail);
    tailPath.closeSubpath();
    bubblePath = bubblePath.united(tailPath);

    // ---- 填充 ----
    painter.setBrush(m_style.background);
    painter.setPen(Qt::NoPen);
    painter.drawPath(bubblePath);

    // ---- 描边 ----
    if (m_style.borderWidth > 0) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_style.border, m_style.borderWidth));
        painter.drawPath(bubblePath);
    }

    // ---- 文本 ----
    painter.setFont(m_style.font);
    painter.setPen(m_style.textColor);

    const QSize content = computeContentSize();
    const QRect textRect(m_style.borderWidth + m_style.paddingHorizontal,
                         m_style.borderWidth + m_style.paddingVertical,
                         content.width(),
                         content.height());

    painter.drawText(textRect,
                     Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop,
                     m_text);

    // ---- 进度指示（三个小圆点依次亮起）----
    if (m_isProgress) {
        const int dotRadius = 3;
        const int dotGap = 10;
        const int totalDots = 3;

        const int startX = textRect.right() - (totalDots * dotGap) + dotGap / 2;
        const int dotY = textRect.center().y() + 2;

        for (int i = 0; i < totalDots; ++i) {
            // 相位决定哪个点最亮，形成"跳动的三点"
            const bool active = ((m_progressPhase / 2) % totalDots) == i;
            QColor color = m_style.textColor;
            color.setAlpha(active ? 230 : 70);
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(QPointF(startX + i * dotGap, dotY),
                                dotRadius, dotRadius);
        }
    }

    // ---- 选项 ----
    if (m_showOptions) {
        for (int i = 0; i < m_optionRects.size(); ++i) {
            const QRect &rect = m_optionRects.at(i);
            const bool hovered = (i == m_hoveredOption);

            painter.setPen(QPen(m_style.optionBorderColor, 1));
            painter.setBrush(hovered ? m_style.optionHoverBackground : Qt::NoBrush);
            painter.drawRoundedRect(rect, 5, 5);

            painter.setPen(m_style.textColor);
            painter.drawText(rect.adjusted(8, 0, -8, 0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             m_options.at(i));
        }
    }
}

// ---------------------------------------------------------------------------
// 鼠标
// ---------------------------------------------------------------------------
void SpeechBubble::mousePressEvent(QMouseEvent *event)
{
    if (!m_showOptions || event->button() != Qt::LeftButton) {
        // 无选项的气泡理论上收不到事件（透明穿透），
        // 但真收到了就当作"关闭"处理 —— 点一下关掉很符合直觉。
        dismissImmediately();
        return;
    }

    const QPoint pos = event->position().toPoint();

    for (int i = 0; i < m_optionRects.size(); ++i) {
        if (m_optionRects.at(i).contains(pos)) {
            const QString chosen = m_options.at(i);

            DC_INFO(LogCat::Ui,
                    QStringLiteral("气泡选项被选中：%1").arg(chosen));

            // 先收起来再发信号，避免调用方在处理信号时
            // 又调 dismiss 造成状态混乱。
            dismissImmediately();
            emit optionChosen(chosen);
            return;
        }
    }

    // 点在空白处：关掉（用户想让它消失）
    dismissImmediately();
}

void SpeechBubble::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_showOptions) {
        return;
    }

    const QPoint pos = event->position().toPoint();
    int hovered = -1;

    for (int i = 0; i < m_optionRects.size(); ++i) {
        if (m_optionRects.at(i).contains(pos)) {
            hovered = i;
            break;
        }
    }

    if (hovered != m_hoveredOption) {
        m_hoveredOption = hovered;
        // 手型光标提示"这里可以点"
        setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void SpeechBubble::enterEvent(QEnterEvent *event)
{
    Q_UNUSED(event)
    m_hovered = true;
}

void SpeechBubble::leaveEvent(QEvent *event)
{
    Q_UNUSED(event)
    m_hovered = false;
    m_hoveredOption = -1;
    setCursor(Qt::ArrowCursor);
    update();
}

} // namespace dc
