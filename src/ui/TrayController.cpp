// ============================================================================
//  TrayController.cpp — 系统托盘图标与菜单实现
// ============================================================================
#include "ui/TrayController.h"

#include "core/Logger.h"
#include "ui/I18n.h"

#include <QAction>
#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSystemTrayIcon>

namespace dc {

namespace {

// 托盘图标配色（按状态）
struct PetPalette {
    QColor body;
    QColor accent;
    QColor eye;
};

PetPalette paletteForState(PetState state)
{
    switch (state) {
    case PetState::Idle:
        return { QColor(0x6F, 0xB1, 0xE0), QColor(0x4A, 0x8F, 0xC4), QColor(0x20, 0x30, 0x40) };
    case PetState::Talking:
        return { QColor(0x7E, 0xC8, 0xA0), QColor(0x55, 0xA8, 0x7C), QColor(0x1B, 0x33, 0x27) };
    case PetState::Working:
        return { QColor(0xF0, 0xC0, 0x60), QColor(0xD0, 0x9E, 0x30), QColor(0x3A, 0x2C, 0x08) };
    case PetState::Success:
        return { QColor(0x8B, 0xD0, 0x8B), QColor(0x5A, 0xA8, 0x5A), QColor(0x14, 0x30, 0x14) };
    case PetState::Failure:
        return { QColor(0xE0, 0x8B, 0x8B), QColor(0xC0, 0x5A, 0x5A), QColor(0x38, 0x10, 0x10) };
    case PetState::Listening:
        return { QColor(0xB8, 0x9B, 0xE0), QColor(0x94, 0x70, 0xC8), QColor(0x24, 0x18, 0x38) };
    }
    return { QColor(0x6F, 0xB1, 0xE0), QColor(0x4A, 0x8F, 0xC4), QColor(0x20, 0x30, 0x40) };
}

// 状态 -> 提示文字 key
const char *stateTooltipKey(PetState state)
{
    switch (state) {
    case PetState::Idle:      return "tray.tooltip.idle";
    case PetState::Talking:   return "tray.tooltip.talking";
    case PetState::Working:   return "tray.tooltip.working";
    case PetState::Success:   return "tray.tooltip.success";
    case PetState::Failure:   return "tray.tooltip.failure";
    case PetState::Listening: return "tray.tooltip.listening";
    }
    return "tray.tooltip.idle";
}

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
TrayController::TrayController(QObject *parent)
    : QObject(parent)
{
}

TrayController::~TrayController()
{
    // 托盘图标必须在 QApplication 还活着的时候销毁，否则有些平台会警告/崩溃。
    //  这里显式 delete 而不是交给 parent，是因为 QSystemTrayIcon 在部分
    //  Qt 版本里对析构时机比较敏感。
    if (m_tray) {
        m_tray->hide();
        delete m_tray;
        m_tray = nullptr;
    }
    // m_menu 的 parent 是 widget-less（构造时没给 parent），
    //  所以由 QSystemTrayIcon 或这里负责。给了 parent 就不重复删。
}

// ---------------------------------------------------------------------------
// 系统托盘可用性
// ---------------------------------------------------------------------------
bool TrayController::isSystemTraySupported(QString *reason)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        if (reason) {
            *reason = I18n::instance().text("tray.unavailable.no_service");
        }
        return false;
    }

    if (!QSystemTrayIcon::supportsMessages()) {
        // 支持托盘但不支持气泡消息：这不是致命问题，只影响通知方式。
        //  返回 true 但记一条日志，让"为什么我没看到托盘通知"有据可查。
        DC_INFO(LogCat::Ui,
                QStringLiteral("System tray available but messages unsupported"));
    }

    return true;
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
bool TrayController::initialize(const TrayMenuState &state)
{
    m_state = state;

    m_available = isSystemTraySupported(&m_unavailableReason);
    if (!m_available) {
        // SRS 6.2：不可用要说明原因。这里既记日志也保留给界面查询。
        DC_WARN(LogCat::Ui,
                QStringLiteral("System tray unavailable: %1")
                    .arg(m_unavailableReason));
        return false;
    }

    if (!m_tray) {
        m_tray = new QSystemTrayIcon(this);
        // 注意：QSystemTrayIcon 用 this 做 parent，生命周期跟 TrayController
        //  一致，但下面析构时仍然显式 delete —— 双保险，代价只是把指针置空。

        buildMenu();

        connect(m_tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    switch (reason) {
                    case QSystemTrayIcon::Trigger:
                        // 单击：切换桌宠显示（最常见的意图）
                        emit togglePetRequested();
                        break;
                    case QSystemTrayIcon::DoubleClick:
                        // 双击：打开输入框（比"再点一次还要找菜单"快得多）
                        emit openInputRequested();
                        break;
                    case QSystemTrayIcon::MiddleClick:
                        // 中键：切换语音输入。这个键位和很多工具的直觉一致。
                        if (m_state.canUseVoice)
                            emit voiceInputRequested();
                        break;
                    case QSystemTrayIcon::Context:
                        // 右键由 Qt 自动弹 m_menu，不需要处理
                        break;
                    case QSystemTrayIcon::Unknown:
                    default:
                        break;
                    }
                });
    }

    applyMenuState();
    refreshIcon();

    DC_INFO(LogCat::Ui, QStringLiteral("TrayController initialized"));

    return true;
}

bool TrayController::isAvailable() const
{
    return m_available;
}

QString TrayController::unavailableReason() const
{
    return m_unavailableReason;
}

// ---------------------------------------------------------------------------
// 菜单
// ---------------------------------------------------------------------------
void TrayController::buildMenu()
{
    if (!m_tray)
        return;

    m_menu = new QMenu();

    m_togglePetAction = m_menu->addAction(QString());
    connect(m_togglePetAction, &QAction::triggered,
            this, &TrayController::togglePetRequested);

    m_openInputAction = m_menu->addAction(QString());
    connect(m_openInputAction, &QAction::triggered,
            this, &TrayController::openInputRequested);

    m_menu->addSeparator();

    m_voiceInputAction = m_menu->addAction(QString());
    connect(m_voiceInputAction, &QAction::triggered,
            this, &TrayController::voiceInputRequested);

    m_recordAction = m_menu->addAction(QString());
    connect(m_recordAction, &QAction::triggered,
            this, &TrayController::recordToggleRequested);

    m_menu->addSeparator();

    m_openHistoryAction = m_menu->addAction(QString());
    connect(m_openHistoryAction, &QAction::triggered,
            this, &TrayController::openHistoryRequested);

    m_openSettingsAction = m_menu->addAction(QString());
    connect(m_openSettingsAction, &QAction::triggered,
            this, &TrayController::openSettingsRequested);

    m_reloadRulesAction = m_menu->addAction(QString());
    connect(m_reloadRulesAction, &QAction::triggered,
            this, &TrayController::reloadRulesRequested);

    m_openDataDirAction = m_menu->addAction(QString());
    connect(m_openDataDirAction, &QAction::triggered,
            this, &TrayController::openDataDirectoryRequested);

    m_menu->addSeparator();

    m_quitAction = m_menu->addAction(QString());
    connect(m_quitAction, &QAction::triggered,
            this, &TrayController::quitRequested);

    // 首次应用一次文案，避免菜单出现空条目
    applyMenuState();

    m_tray->setContextMenu(m_menu);
}

// ---------------------------------------------------------------------------
// 刷新菜单状态
// ---------------------------------------------------------------------------
void TrayController::setMenuState(const TrayMenuState &state)
{
    m_state = state;
    applyMenuState();
    refreshIcon();
}

TrayMenuState TrayController::menuState() const
{
    return m_state;
}

void TrayController::applyMenuState()
{
    if (!m_available || !m_menu)
        return;

    const I18n &i18n = I18n::instance();

    // ---- 显示/隐藏桌宠 ----
    if (m_togglePetAction) {
        // 文案由调用方通过 petVisibleText 给出当前状态对应的动作名
        m_togglePetAction->setText(
            m_state.petVisibleText.isEmpty()
                ? i18n.text("tray.menu.toggle_pet")
                : m_state.petVisibleText);
        m_togglePetAction->setEnabled(m_state.canTogglePet);
    }

    // ---- 输入框 ----
    if (m_openInputAction) {
        m_openInputAction->setText(i18n.text("tray.menu.open_input"));
        m_openInputAction->setEnabled(m_state.canOpenInput);
        m_openInputAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+L")));
    }

    // ---- 语音输入 ----
    if (m_voiceInputAction) {
        m_voiceInputAction->setText(i18n.text("tray.menu.voice_input"));
        m_voiceInputAction->setEnabled(m_state.canUseVoice);
        // SRS 6.2：禁用的菜单项要有解释。tooltip 是最合适的位置 ——
        //  它不占菜单宽度，用户悬停就能看到原因。
        m_voiceInputAction->setToolTip(
            m_state.canUseVoice
                ? i18n.text("tray.tooltip.voice_ready")
                : (m_state.voiceDisabledReason.isEmpty()
                       ? i18n.text("tray.disabled.unknown")
                       : m_state.voiceDisabledReason));
    }

    // ---- 录音 ----
    if (m_recordAction) {
        m_recordAction->setText(i18n.text("tray.menu.record"));
        m_recordAction->setEnabled(m_state.canRecord);
        m_recordAction->setToolTip(
            m_state.canRecord
                ? i18n.text("tray.tooltip.record_ready")
                : (m_state.recordDisabledReason.isEmpty()
                       ? i18n.text("tray.disabled.unknown")
                       : m_state.recordDisabledReason));
    }

    // ---- 历史 ----
    if (m_openHistoryAction) {
        m_openHistoryAction->setText(i18n.text("tray.menu.history"));
        m_openHistoryAction->setEnabled(m_state.canOpenHistory);
        m_openHistoryAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+H")));
    }

    // ---- 设置 ----
    if (m_openSettingsAction) {
        m_openSettingsAction->setText(i18n.text("tray.menu.settings"));
        m_openSettingsAction->setEnabled(m_state.canOpenSettings);
    }

    // ---- 重载规则 ----
    if (m_reloadRulesAction) {
        m_reloadRulesAction->setText(i18n.text("tray.menu.reload_rules"));
        m_reloadRulesAction->setToolTip(i18n.text("tray.tooltip.reload_rules"));
    }

    // ---- 打开数据目录 ----
    if (m_openDataDirAction) {
        m_openDataDirAction->setText(i18n.text("tray.menu.open_data_dir"));
    }

    // ---- 退出 ----
    if (m_quitAction) {
        m_quitAction->setText(i18n.text("tray.menu.quit"));
    }

    // 全局快捷键不可用时，在提示里说明 —— 否则用户会反复按快捷键却毫无反应
    if (!m_state.hotkeyDisabledReason.isEmpty()) {
        if (m_openInputAction)
            m_openInputAction->setToolTip(m_state.hotkeyDisabledReason);
    }
}

// ---------------------------------------------------------------------------
// 图标
// ---------------------------------------------------------------------------
void TrayController::refreshIcon()
{
    if (!m_available || !m_tray)
        return;

    const QPixmap icon = buildTrayIcon(64, m_currentPetState, m_state.petImagePath);
    m_tray->setIcon(QIcon(icon));

    const QString appName = m_state.appName.isEmpty()
                                ? I18n::instance().text("app.name")
                                : m_state.appName;

    // 提示文字里带上当前状态，让用户不用打开窗口就知道程序在做什么
    m_tray->setToolTip(QStringLiteral("%1 — %2")
                           .arg(appName,
                                I18n::instance().text(stateTooltipKey(m_currentPetState))));
}

QPixmap TrayController::buildTrayIcon(int size,
                                      PetState state,
                                      const QString &customImagePath)
{
    // 尺寸兜底：托盘图标太小看不清，太大浪费内存
    if (size < 16)
        size = 16;
    if (size > 256)
        size = 256;

    // ---- 用户自定义图标优先 ----
    if (!customImagePath.isEmpty()) {
        QPixmap custom(customImagePath);
        if (!custom.isNull()) {
            return custom.scaled(size, size,
                                 Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        }
        // 自定义图标加载失败时不要返回空 pixmap ——
        //  那会让托盘显示白方块。落到下面画默认图标。
        DC_WARN(LogCat::Ui,
                QStringLiteral("Custom tray icon failed to load, using built-in"));
    }

    const PetPalette pal = paletteForState(state);

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal s = static_cast<qreal>(size);
    const QRectF bodyRect(s * 0.12, s * 0.16, s * 0.76, s * 0.72);
    const qreal radius = s * 0.26;

    // ---- 身体 ----
    QPainterPath body;
    body.addRoundedRect(bodyRect, radius, radius);
    p.fillPath(body, pal.body);
    p.setPen(QPen(pal.accent, qMax(1.0, s * 0.045)));
    p.drawPath(body);

    // ---- 眼睛 ----
    const qreal eyeR = s * 0.075;
    const qreal eyeY = bodyRect.top() + bodyRect.height() * 0.42;
    const qreal eyeLx = bodyRect.left() + bodyRect.width() * 0.32;
    const qreal eyeRx = bodyRect.left() + bodyRect.width() * 0.68;

    p.setPen(Qt::NoPen);
    p.setBrush(pal.eye);

    if (state == PetState::Success) {
        // 成功：画成弯月形笑眼（用两条弧线）
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(pal.eye, qMax(1.5, s * 0.065), Qt::SolidLine, Qt::RoundCap));
        for (qreal cx : { eyeLx, eyeRx }) {
            QRectF arc(cx - eyeR * 1.6, eyeY - eyeR * 1.2,
                       eyeR * 3.2, eyeR * 2.6);
            p.drawArc(arc, 20 * 16, 140 * 16);
        }
    } else if (state == PetState::Failure) {
        // 失败：画成下垂的短横线
        p.setPen(QPen(pal.eye, qMax(1.5, s * 0.07), Qt::SolidLine, Qt::RoundCap));
        for (qreal cx : { eyeLx, eyeRx }) {
            p.drawLine(QPointF(cx - eyeR * 1.3, eyeY + eyeR * 0.4),
                       QPointF(cx + eyeR * 1.3, eyeY - eyeR * 0.4));
        }
    } else if (state == PetState::Working) {
        // 工作：眼睛缩成一条横线（专注）
        p.setPen(QPen(pal.eye, qMax(1.5, s * 0.075), Qt::SolidLine, Qt::RoundCap));
        for (qreal cx : { eyeLx, eyeRx }) {
            p.drawLine(QPointF(cx - eyeR * 1.4, eyeY),
                       QPointF(cx + eyeR * 1.4, eyeY));
        }
    } else if (state == PetState::Listening) {
        // 监听：眼睛放大（警觉）
        for (qreal cx : { eyeLx, eyeRx })
            p.drawEllipse(QPointF(cx, eyeY), eyeR * 1.35, eyeR * 1.35);
    } else {
        // 待机 / 说话：正常圆眼
        for (qreal cx : { eyeLx, eyeRx })
            p.drawEllipse(QPointF(cx, eyeY), eyeR, eyeR);
    }

    // ---- 状态点缀 ----
    if (state == PetState::Talking) {
        // 说话：嘴边画一个小三角（张合的嘴是"正在说话"的通用符号）
        p.setPen(Qt::NoPen);
        p.setBrush(pal.eye);
        const qreal mouthY = bodyRect.top() + bodyRect.height() * 0.68;
        const qreal mouthW = s * 0.11;
        QPainterPath mouth;
        mouth.moveTo(bodyRect.center().x() - mouthW / 2, mouthY);
        mouth.lineTo(bodyRect.center().x() + mouthW / 2, mouthY);
        mouth.lineTo(bodyRect.center().x(), mouthY + mouthW * 0.75);
        mouth.closeSubpath();
        p.fillPath(mouth, pal.eye);
    } else if (state == PetState::Listening) {
        // 监听：右下角一个小红点（"正在录/正在听"）
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xE0, 0x3A, 0x3A));
        p.drawEllipse(QPointF(s * 0.80, s * 0.80), s * 0.11, s * 0.11);
    }

    p.end();
    return pm;
}

// ---------------------------------------------------------------------------
// 显示 / 隐藏
// ---------------------------------------------------------------------------
void TrayController::show()
{
    if (!m_available || !m_tray)
        return;

    m_tray->show();
    DC_INFO(LogCat::Ui, QStringLiteral("Tray icon shown"));
}

void TrayController::hide()
{
    if (!m_tray)
        return;

    m_tray->hide();
}

// ---------------------------------------------------------------------------
// 弹出上下文菜单
//
//  与托盘图标右键的菜单是同一个对象（m_menu）。
//  这样"菜单里有什么"只有一个来源 —— 不会出现桌宠右键少了某一项
//  而用户以为是 bug 的情况。
// ---------------------------------------------------------------------------
void TrayController::showContextMenu(const QPoint &globalPos)
{
    if (!m_menu)
        return;

    DC_INFO(LogCat::Ui, QStringLiteral("Context menu requested"));

    m_menu->popup(globalPos);
}

// ---------------------------------------------------------------------------
// 通知
// ---------------------------------------------------------------------------
void TrayController::notify(const QString &title,
                            const QString &message,
                            bool warning)
{
    if (!m_available || !m_tray)
        return;

    if (!QSystemTrayIcon::supportsMessages()) {
        // 系统不支持托盘气泡：静默跳过。
        //  这不是错误 —— 桌宠气泡已经把消息给用户了，这里只是补一条。
        DC_INFO(LogCat::Ui,
                QStringLiteral("Tray notification skipped (unsupported): %1")
                    .arg(title));
        return;
    }

    m_tray->showMessage(title.isEmpty() ? m_state.appName : title,
                        message,
                        warning ? QSystemTrayIcon::Warning
                                : QSystemTrayIcon::Information,
                        5000);
}

} // namespace dc
