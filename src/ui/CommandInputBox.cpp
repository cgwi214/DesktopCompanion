// ============================================================================
//  CommandInputBox.cpp — 命令输入框实现
// ============================================================================
#include "ui/CommandInputBox.h"

#include "core/Logger.h"

#include <QApplication>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QStyleOption>
#include <QVBoxLayout>

namespace dc {

namespace {

// 圆角与配色（浅色主题，和气泡保持一致的视觉语言）
constexpr int kCornerRadius = 12;
constexpr int kBorderWidth = 1;
constexpr int kMargin = 14;
constexpr int kSpacing = 8;

const QColor kPanelBg(0xFF, 0xFF, 0xFF, 250);
const QColor kPanelBorder(0x90, 0x9A, 0xA8, 220);
const QColor kHintNormal(0x55, 0x5F, 0x6D);
const QColor kHintError(0xC0, 0x39, 0x2B);

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
CommandInputBox::CommandInputBox(QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    // 说明：这里刻意**不**设 Qt::WindowDoesNotAcceptFocus。
    //  气泡不抢焦点是对的，但输入框存在的意义就是让用户打字。
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_DeleteOnClose, false);   // 复用同一个实例，避免反复创建

    m_config.placeholder = I18n::instance().text("input.placeholder");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
    root->setSpacing(kSpacing);

    // ---- 第一行：输入框 + 语音按钮 + 提交按钮 ----
    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(kSpacing);

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(m_config.placeholder);
    m_input->setClearButtonEnabled(true);
    m_input->setFrame(false);
    m_input->setMinimumHeight(32);
    connect(m_input, &QLineEdit::returnPressed,
            this, &CommandInputBox::trySubmit);
    connect(m_input, &QLineEdit::textChanged, this, [this](const QString &) {
        refreshCounter();
        // 一旦开始打字就退出历史浏览状态，否则用户会以为输入被"粘住"了
        m_historyCursor = -1;
    });
    inputRow->addWidget(m_input, 1);

    m_voiceButton = new QPushButton(QStringLiteral("🎤"), this);
    m_voiceButton->setFixedSize(34, 32);
    m_voiceButton->setToolTip(I18n::instance().text("input.voice.tooltip"));
    m_voiceButton->setCursor(Qt::PointingHandCursor);
    connect(m_voiceButton, &QPushButton::clicked,
            this, &CommandInputBox::voiceInputRequested);
    inputRow->addWidget(m_voiceButton);

    m_submitButton = new QPushButton(I18n::instance().text("input.submit"), this);
    m_submitButton->setMinimumHeight(32);
    m_submitButton->setCursor(Qt::PointingHandCursor);
    m_submitButton->setDefault(true);
    connect(m_submitButton, &QPushButton::clicked,
            this, &CommandInputBox::trySubmit);
    inputRow->addWidget(m_submitButton);

    root->addLayout(inputRow);

    // ---- 第二行：提示 + 字数 ----
    auto *metaRow = new QHBoxLayout;
    metaRow->setSpacing(kSpacing);

    m_hintLabel = new QLabel(this);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setVisible(false);
    metaRow->addWidget(m_hintLabel, 1);

    m_counterLabel = new QLabel(this);
    m_counterLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_counterLabel->setStyleSheet(QStringLiteral("color:#8A94A2;"));
    metaRow->addWidget(m_counterLabel, 0);

    root->addLayout(metaRow);

    // ---- 建议列表（历史回看时显示，默认隐藏）----
    m_suggestionList = new QListWidget(this);
    m_suggestionList->setVisible(false);
    m_suggestionList->setMaximumHeight(150);
    m_suggestionList->setFrameShape(QFrame::NoFrame);
    connect(m_suggestionList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                if (!item) return;
                m_input->setText(item->text());
                m_input->setFocus();
            });
    root->addWidget(m_suggestionList);

    refreshCounter();
    resize(m_config.width, m_config.minHeight);
}

CommandInputBox::~CommandInputBox() = default;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void CommandInputBox::setConfig(const CommandInputConfig &config)
{
    m_config = config;
    if (m_config.maxInputLength <= 0)
        m_config.maxInputLength = 512;   // 兜底：配置坏了也不能变成"什么都输不了"
    if (m_config.placeholder.isEmpty())
        m_config.placeholder = I18n::instance().text("input.placeholder");

    m_input->setPlaceholderText(m_config.placeholder);
    // QLineEdit 自身也设一遍上限，双保险：即使有人绕过 trySubmit 也截得住
    m_input->setMaxLength(m_config.maxInputLength);
    m_counterLabel->setVisible(m_config.showCharCounter);

    refreshCounter();
    relayout(m_anchor);
}

CommandInputConfig CommandInputBox::config() const
{
    return m_config;
}

// ---------------------------------------------------------------------------
// 唤出 / 收起
// ---------------------------------------------------------------------------
void CommandInputBox::summon(const QRect &anchor)
{
    m_anchor = anchor;

    // 每次唤出都是干净状态：清空输入、清掉上次的提示与历史浏览位置
    m_input->clear();
    clearHint();
    m_historyCursor = -1;
    m_draftBeforeHistory.clear();

    relayout(anchor);

    show();
    raise();
    activateWindow();
    m_input->setFocus(Qt::ShortcutFocusReason);

    m_summoned = true;

    DC_INFO(LogCat::Ui, QStringLiteral("CommandInputBox summoned"));
}

void CommandInputBox::dismiss()
{
    if (!m_summoned)
        return;

    m_summoned = false;
    m_historyCursor = -1;
    m_draftBeforeHistory.clear();
    hide();

    DC_INFO(LogCat::Ui, QStringLiteral("CommandInputBox dismissed"));
}

bool CommandInputBox::isSummoned() const
{
    return m_summoned;
}

// ---------------------------------------------------------------------------
// 语音塞入
// ---------------------------------------------------------------------------
void CommandInputBox::setTextAndSubmit(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        DC_WARN(LogCat::Ui,
                QStringLiteral("setTextAndSubmit called with empty text, ignored"));
        return;
    }

    // 如果已经很干净地打开着，就不重复 summon（避免把用户正在打的内容清掉）
    if (!m_summoned)
        summon(m_anchor);

    m_input->setText(trimmed);
    trySubmit();
}

// ---------------------------------------------------------------------------
// 文本访问
// ---------------------------------------------------------------------------
QString CommandInputBox::currentText() const
{
    return m_input ? m_input->text() : QString();
}

void CommandInputBox::rememberInput(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    // 重复的输入提到最前，而不是堆两条一样的
    m_history.removeAll(trimmed);
    m_history.prepend(trimmed);

    while (m_history.size() > m_config.maxSessionHistory)
        m_history.removeLast();
}

QStringList CommandInputBox::sessionHistory() const
{
    return m_history;
}

// ---------------------------------------------------------------------------
// 提示区
// ---------------------------------------------------------------------------
void CommandInputBox::showHint(const QString &hint, bool isError)
{
    if (!m_hintLabel)
        return;

    if (hint.isEmpty()) {
        m_hintLabel->setVisible(false);
        relayout(m_anchor);
        return;
    }

    m_hintLabel->setText(hint);
    m_hintLabel->setStyleSheet(
        QStringLiteral("color:%1;")
            .arg(isError ? kHintError.name() : kHintNormal.name()));
    m_hintLabel->setVisible(true);
    relayout(m_anchor);
}

void CommandInputBox::clearHint()
{
    if (m_hintLabel)
        m_hintLabel->setVisible(false);
}

// ---------------------------------------------------------------------------
// 提交
// ---------------------------------------------------------------------------
void CommandInputBox::trySubmit()
{
    const QString text = m_input->text().trimmed();

    // 空输入不算错误，只是什么都不做 —— 弹"请输入内容"是多余的打扰
    if (text.isEmpty()) {
        showHint(I18n::instance().text("input.hint.empty"), false);
        return;
    }

    if (text.length() > m_config.maxInputLength) {
        // 正常情况下 QLineEdit 已经拦住了，这里处理"程序化塞进来的超长文本"
        const QString msg = I18n::instance().text(
            "input.hint.too_long",
            QStringLiteral("max"), QString::number(m_config.maxInputLength));
        showHint(msg, true);
        DC_WARN(LogCat::Ui,
                QStringLiteral("Input rejected: length %1 exceeds limit %2")
                    .arg(text.length()).arg(m_config.maxInputLength));
        return;
    }

    clearHint();
    rememberInput(text);

    // 收起（而不是保持打开）：一次交互一个来回，符合"轻量助手"的定位
    dismiss();

    DC_INFO(LogCat::Ui,
            QStringLiteral("Command submitted: length=%1").arg(text.length()));

    emit submitted(text);
}

// ---------------------------------------------------------------------------
// 会话内历史回看
// ---------------------------------------------------------------------------
void CommandInputBox::navigateHistory(int delta)
{
    if (m_history.isEmpty())
        return;

    // 第一次进入浏览：把用户当前的草稿存起来，回到尽头时还给他
    if (m_historyCursor < 0) {
        if (delta > 0) {
            // 已经是最新端，再往上没有东西了
            return;
        }
        m_draftBeforeHistory = m_input->text();
        m_historyCursor = 0;
    } else {
        m_historyCursor += delta;
    }

    if (m_historyCursor < 0) {
        // 越过最新一条：回到草稿
        m_historyCursor = -1;
        m_input->setText(m_draftBeforeHistory);
        m_draftBeforeHistory.clear();
        return;
    }

    if (m_historyCursor >= m_history.size()) {
        // 越界就停在最旧一条，不要循环 —— 循环会让人失去方向感
        m_historyCursor = m_history.size() - 1;
    }

    const QString value = m_history.at(m_historyCursor);
    m_input->setText(value);
    m_input->setCursorPosition(value.length());
}

// ---------------------------------------------------------------------------
// 字数提示
// ---------------------------------------------------------------------------
void CommandInputBox::refreshCounter()
{
    if (!m_counterLabel)
        return;

    if (!m_config.showCharCounter) {
        m_counterLabel->setVisible(false);
        return;
    }

    const int len = m_input->text().length();
    const int maxLen = m_config.maxInputLength;

    m_counterLabel->setText(QStringLiteral("%1 / %2").arg(len).arg(maxLen));
    m_counterLabel->setVisible(true);

    // 接近上限时变黄、达到上限变红 —— 让用户提前知道而不是被拒绝
    QString color = QStringLiteral("#8A94A2");
    if (len >= maxLen)
        color = kHintError.name();
    else if (maxLen > 0 && len * 10 >= maxLen * 9)
        color = QStringLiteral("#C88A20");

    m_counterLabel->setStyleSheet(
        QStringLiteral("color:%1;").arg(color));
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
void CommandInputBox::relayout(const QRect &anchor)
{
    setFixedWidth(m_config.width);

    // 让布局先算出内容需要多高，再定窗口高度
    if (layout())
        layout()->activate();

    const int hintExtra = (m_hintLabel && m_hintLabel->isVisible())
                              ? m_hintLabel->sizeHint().height() : 0;
    const int suggestionExtra =
        (m_suggestionList && m_suggestionList->isVisible())
            ? m_suggestionList->height() : 0;

    int h = kMargin * 2 + 32 + kSpacing + 18;   // 上下边距 + 输入行 + 间距 + 元信息行
    h += hintExtra > 0 ? hintExtra + kSpacing : 0;
    h += suggestionExtra > 0 ? suggestionExtra + kSpacing : 0;

    resize(m_config.width, qMax(m_config.minHeight, h));

    const QSize sz(size());
    move(computePosition(anchor, sz));
}

QPoint CommandInputBox::computePosition(const QRect &anchor,
                                       const QSize &size) const
{
    // 找到锚点所在的屏幕（多显示器下不能假设主屏）
    QScreen *screen = nullptr;
    if (anchor.isValid()) {
        screen = QGuiApplication::screenAt(anchor.center());
    }
    if (!screen)
        screen = QGuiApplication::primaryScreen();

    const QRect avail = screen ? screen->availableGeometry()
                               : QRect(0, 0, 1280, 720);

    // 屏幕中央：这是"没有锚点"时的默认位置，也是视觉上最稳的位置
    QPoint pos(avail.center().x() - size.width() / 2,
               avail.center().y() - size.height() / 2);

    if (anchor.isValid()) {
        // 有桌宠锚点时，优先显示在桌宠上方并水平对齐
        pos.setX(anchor.center().x() - size.width() / 2);

        const int aboveY = anchor.top() - size.height() - 12;
        if (aboveY >= avail.top()) {
            pos.setY(aboveY);
        } else {
            // 上方放不下就放下面；放下面也放不下时才退回屏幕中央
            const int belowY = anchor.bottom() + 12;
            if (belowY + size.height() <= avail.bottom())
                pos.setY(belowY);
        }
    }

    // 无论怎么算，最后都夹进可用区域 —— 绝不允许窗口跑到屏幕外
    pos.setX(qBound(avail.left(), pos.x(), avail.right() - size.width() + 1));
    pos.setY(qBound(avail.top(), pos.y(), avail.bottom() - size.height() + 1));

    return pos;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void CommandInputBox::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    // 圆角底：半透明背景需要先自己画一个形状，
    // 否则 Qt 会把整个矩形当成窗口内容（WA_TranslucentBackground 的代价）
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    QPainterPath path;
    path.addRoundedRect(r, kCornerRadius, kCornerRadius);

    p.fillPath(path, kPanelBg);
    p.setPen(QPen(kPanelBorder, kBorderWidth));
    p.drawPath(path);

    // 输入框下的一条细分隔线，提示"这里可以打字"
    if (m_input) {
        const QRect inputGeom = m_input->geometry();
        p.setPen(QPen(QColor(0xD0, 0xD6, 0xDE), 1));
        p.drawLine(inputGeom.left(), inputGeom.bottom() + 1,
                   inputGeom.right(), inputGeom.bottom() + 1);
    }
}

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------
void CommandInputBox::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        // SRS 4.12：Esc 取消当前输入。取消就是彻底取消 —— 不提交、不留历史。
        dismiss();
        emit cancelled();
        return;

    case Qt::Key_Up:
        navigateHistory(1);      // 往更旧的方向
        event->accept();
        return;

    case Qt::Key_Down:
        navigateHistory(-1);     // 往更新的方向
        event->accept();
        return;

    case Qt::Key_Return:
    case Qt::Key_Enter:
        trySubmit();
        return;

    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

// ---------------------------------------------------------------------------
// 鼠标拖动
//
//  这个面板是无边框的，用户可能想把它拖到顺手的位置。
//  实现和桌宠窗口一样用自己算偏移，保持代码风格一致。
// ---------------------------------------------------------------------------
void CommandInputBox::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // 只有点击"空白面板区域"才拖动。点在控件上应交给控件处理
        // （否则用户想选中输入框里的文字，结果把整个窗口拖走了）。
        QWidget *child = childAt(event->pos());
        if (!child || child == this) {
            m_dragging = true;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void CommandInputBox::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void CommandInputBox::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

// ---------------------------------------------------------------------------
// 失焦
//
//  失焦不自动关闭。原因：用户可能要点桌宠或别的窗口看一眼再回来继续打，
//  自动关闭会把已经打了一半的内容弄丢。想关就按 Esc。
// ---------------------------------------------------------------------------
void CommandInputBox::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
}

} // namespace dc
