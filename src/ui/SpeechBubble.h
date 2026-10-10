// ============================================================================
//  SpeechBubble.h — 对话气泡
//
//  需求依据：SRS V1.1 4.2
//    TXT-002：桌宠以气泡形式展示回复
//    TXT-005：气泡不得遮挡关键操作区域（避免全屏遮挡）
//    4.2 澄清：低置信度时需要展示选项（SRS 4.6）
//
//  关键设计决定（为什么不用 QLabel 简单了事）：
//
//    1. 【必须是自己画的窗口，不能是子控件】
//       桌宠主窗口是无边框小窗口，气泡文字长度不可预测。
//       如果做成子控件，长文本会被主窗口裁掉；
//       做成独立窗口才能自适应大小。
//
//    2. 【不抢焦点】
//       气泡是个提示，不应该抢走用户正在输入的东西。
//       因此用 Qt::WindowDoesNotAcceptFocus，并且不设 Qt::WindowActive。
//       这一点很容易做错 —— 一旦气泡抢焦点，用户的输入框就会失焦。
//
//    3. 【不接收鼠标事件（可选）】
//       默认点击穿透，用户点气泡等于点下面的桌面。
//       但"有选项的澄清气泡"必须能点，所以提供 setInteractive()。
//
//    4. 【自动避开屏幕边缘】
//       气泡默认在桌宠上方；如果桌宠贴近屏幕顶部，
//       气泡自动翻到下方（TXT-005：不遮挡关键区域）。
//
//  生命周期：
//      showMessage(text)            普通提示，超时自动关闭
//      showMessage(text, durationMs) 自定义时长（0 = 不自动关闭）
//      showChoices(prompt, options)  带可点选项（澄清用），必须用户选择
// ============================================================================
#pragma once

#include <QColor>
#include <QFont>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace dc {

// ---------------------------------------------------------------------------
// 气泡样式（由配置/主题决定）
// ---------------------------------------------------------------------------
struct BubbleStyle {
    QColor background = QColor(0xFF, 0xFF, 0xFF, 240);
    QColor border = QColor(0x50, 0x50, 0x50, 180);
    QColor textColor = QColor(0x20, 0x20, 0x20);
    QColor optionHoverBackground = QColor(0xE8, 0xF2, 0xFC);
    QColor optionBorderColor = QColor(0xC8, 0xC8, 0xC8);

    int borderWidth = 1;
    int cornerRadius = 10;
    int paddingHorizontal = 12;
    int paddingVertical = 9;

    int maxWidth = 360;         // 最大宽度，超过则换行（SRS 5 配置项）
    int tailHeight = 8;         // 指向桌宠的小三角高度
    int tailWidth = 14;

    QFont font;                 // 空字体 = 用默认
    int lineSpacing = 2;
};

// ---------------------------------------------------------------------------
// SpeechBubble
// ---------------------------------------------------------------------------
class SpeechBubble : public QWidget
{
    Q_OBJECT

public:
    explicit SpeechBubble(QWidget *parent = nullptr);
    ~SpeechBubble() override;

    void setStyle(const BubbleStyle &style);
    BubbleStyle style() const;

    // -----------------------------------------------------------------------
    // 显示一条消息
    //
    //  anchor          桌宠占用的屏幕矩形（气泡据此定位）
    //  durationMs      自动关闭的毫秒数；<= 0 表示不自动关闭
    //  autoCloseOnHover 鼠标悬停时暂停倒计时（默认开启，便于阅读长文本）
    // -----------------------------------------------------------------------
    void showMessage(const QString &text,
                     const QRect &anchor,
                     int durationMs = 4000,
                     bool autoCloseOnHover = true);

    // -----------------------------------------------------------------------
    // 显示带选项的消息（澄清场景，SRS 4.6）
    //
    //  用户必须先选一个，不会自动关闭 —— 因为自动关闭等于
    //  "悄悄替你做了决定"，这在需要澄清时是错的。
    // -----------------------------------------------------------------------
    void showChoices(const QString &prompt,
                     const QStringList &options,
                     const QRect &anchor);

    // 显示"正在执行"状态（带一个简单的活动指示）
    void showProgress(const QString &text, const QRect &anchor);

    // 关闭（可带动画结束后销毁）
    void dismiss();
    void dismissImmediately();

    // 是否正在显示
    bool isVisibleBubble() const;

    // 当前文本（供测试与历史记录核对）
    QString currentText() const;

    // -----------------------------------------------------------------------
    // 定位
    //
    //  放在 anchor 上方；空间不足则放下方；左右夹在屏幕内。
    //  公开以便单元测试直接验证边界处理。
    // -----------------------------------------------------------------------
    QPoint computePosition(const QRect &anchor,
                           const QSize &bubbleSize,
                           const QRect &availableScreen) const;

signals:
    // 用户点选了某个选项（参数是选项文本）
    void optionChosen(const QString &option);

    // 气泡关闭（无论是超时还是手动）
    void dismissed();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void enterEvent(QEnterEvent *event) override;

private:
    // 根据文本与最大宽度计算气泡内容尺寸
    QSize computeContentSize() const;

    // 重新计算几何（尺寸 + 位置）并 apply
    void relayout();

    // 布局选项矩形（供命中测试）
    void layoutOptionRects();

    // 把窗口尺寸设成"内容尺寸 + 尾巴 + 边框"
    QSize totalSize() const;

    // 选项区域高度
    int optionsHeight() const;

    BubbleStyle m_style;

    QString m_text;
    QStringList m_options;
    bool m_showOptions = false;
    bool m_isProgress = false;

    // 进度动画的相位（0..1）
    int m_progressPhase = 0;

    QRect m_anchor;
    QRect m_availableScreen;

    // 每个选项的矩形（相对本窗口坐标）
    QList<QRect> m_optionRects;
    int m_hoveredOption = -1;

    // 自动关闭
    int m_durationMs = 4000;
    bool m_autoCloseOnHover = true;
    bool m_hovered = false;
    int m_elapsedMs = 0;
    int m_timerId = 0;
    int m_animationTimerId = 0;

    void timerEvent(QTimerEvent *event) override;
};

} // namespace dc
