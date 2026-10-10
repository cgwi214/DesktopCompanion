// ============================================================================
//  RecorderPanel.cpp — 录音面板实现
// ============================================================================
#include "ui/RecorderPanel.h"

#include "core/Logger.h"
#include "services/HistoryService.h"
#include "ui/I18n.h"

#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace dc {

namespace {

// 呼吸点动画间隔
constexpr int kBlinkIntervalMs = 600;

// 录音中的强调色
const char *kRecordingColor = "#C62828";

// 计算百分比并夹到 0..100。
//
// 为什么不用 qBound：qBound<T>(min, value, max) 要求三个实参能推导出同一个 T。
// 这里 value 是 qint64（已用字节数、毫秒数都有可能是大数），而 0 / 100 是 int
// 字面量，MinGW 会报
//     call of overloaded 'qBound<qint64>(int, long long int, int)' is ambiguous
// 所以自己写一个全程 qint64 的比较，最后再收窄成 int。
// 另外先除后乘会丢精度，这里保持「先乘后除」，代价是 value 极大时可能溢出——
// value 来自文件字节数与毫秒数，离 qint64 上限还有十几个数量级，安全。
int percentOf(qint64 value, qint64 limit)
{
    if (limit <= 0)
        return 0;
    const qint64 pct = value * 100 / limit;
    if (pct <= 0)
        return 0;
    if (pct >= 100)
        return 100;
    return static_cast<int>(pct);
}

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
RecorderPanel::RecorderPanel(RecorderService *recorder,
                             HistoryService *history,
                             QWidget *parent)
    : QDialog(parent)
    , m_recorder(recorder)
    , m_history(history)
{
    setWindowTitle(I18n::instance().text("recorder.title"));
    setMinimumSize(600, 480);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    buildUi();

    // ---- 能力探测：决定了整块界面可用性 ----
    if (!m_recorder) {
        m_available = false;
        m_unavailableReason =
            I18n::instance().text("recorder.unavailable.service_missing");
    } else {
        m_unavailableReason.clear();
        m_available = m_recorder->probeCapability(&m_unavailableReason);

        // ---- 连接服务信号 ----
        connect(m_recorder, &RecorderService::stateChanged,
                this, &RecorderPanel::onRecorderStateChanged);
        connect(m_recorder, &RecorderService::recordingStarted,
                this, &RecorderPanel::onRecordingStarted);
        connect(m_recorder, &RecorderService::recordingStopped,
                this, &RecorderPanel::onRecordingStopped);
        connect(m_recorder, &RecorderService::recordingProgress,
                this, &RecorderPanel::onRecordingProgress);
        connect(m_recorder, &RecorderService::limitReached,
                this, &RecorderPanel::onLimitReached);
        connect(m_recorder, &RecorderService::recorderError,
                this, &RecorderPanel::onRecorderError);
    }

    updateCapabilityBanner();
    updateControlsForState(m_recorder ? m_recorder->state()
                                      : RecorderState::Unavailable);
    refreshList();
}

RecorderPanel::~RecorderPanel()
{
    // 面板关闭时如果还在录音：不停（录是服务的事，面板只是界面），
    //  但要断开信号，避免信号打到已经析构的对象上。
    if (m_recorder)
        disconnect(m_recorder, nullptr, this, nullptr);

    if (m_blinkTimer) {
        m_blinkTimer->stop();
        delete m_blinkTimer;
        m_blinkTimer = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------
void RecorderPanel::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 14, 16, 14);
    root->setSpacing(10);

    // ---- 能力提示条 ----
    m_bannerLabel = new QLabel(this);
    m_bannerLabel->setWordWrap(true);
    m_bannerLabel->setVisible(false);
    root->addWidget(m_bannerLabel);

    // ---- 状态行：呼吸点 + 状态文字 + 计时器 ----
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(8);

    m_dotLabel = new QLabel(QStringLiteral("●"), this);
    m_dotLabel->setStyleSheet(QStringLiteral("color:%1; font-size:15px;")
                                  .arg(kRecordingColor));
    m_dotLabel->setVisible(false);
    statusRow->addWidget(m_dotLabel, 0);

    m_statusLabel = new QLabel(this);
    statusRow->addWidget(m_statusLabel, 1);

    m_timerLabel = new QLabel(QStringLiteral("00:00"), this);
    QFont timerFont = m_timerLabel->font();
    timerFont.setPointSizeF(timerFont.pointSizeF() + 6.0);
    timerFont.setBold(true);
    m_timerLabel->setFont(timerFont);
    timerFont = m_timerLabel->font();
    m_timerLabel->setStyleSheet(QStringLiteral("color:%1;").arg(kRecordingColor));
    statusRow->addWidget(m_timerLabel, 0);

    root->addLayout(statusRow);

    // ---- 时长进度 ----
    m_durationBar = new QProgressBar(this);
    m_durationBar->setRange(0, 100);
    m_durationBar->setValue(0);
    m_durationBar->setTextVisible(false);
    m_durationBar->setMaximumHeight(6);
    root->addWidget(m_durationBar);

    // ---- 存储占用 ----
    m_storageBar = new QProgressBar(this);
    m_storageBar->setRange(0, 100);
    m_storageBar->setValue(0);
    m_storageBar->setTextVisible(false);
    m_storageBar->setMaximumHeight(6);
    root->addWidget(m_storageBar);

    m_storageLabel = new QLabel(this);
    m_storageLabel->setStyleSheet(QStringLiteral("color:#7A8593;"));
    root->addWidget(m_storageLabel);

    m_folderLabel = new QLabel(this);
    m_folderLabel->setStyleSheet(QStringLiteral("color:#7A8593;"));
    m_folderLabel->setWordWrap(true);
    m_folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_folderLabel);

    // ---- 操作按钮行 ----
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);

    m_recordButton = new QPushButton(
        I18n::instance().text("recorder.button.start"), this);
    m_recordButton->setMinimumHeight(34);
    connect(m_recordButton, &QPushButton::clicked, this, [this]() {
        if (!m_recorder || !m_available)
            return;

        const RecorderResult r = m_recorder->start();
        if (!r.ok) {
            // 服务层已经给了面向用户的原因，直接展示（SRS 7.2）
            QMessageBox::warning(this,
                                 I18n::instance().text("recorder.title"),
                                 r.message.isEmpty()
                                     ? I18n::instance().text("recorder.error.start_failed")
                                     : r.message);
        }
    });
    buttonRow->addWidget(m_recordButton);

    m_stopButton = new QPushButton(
        I18n::instance().text("recorder.button.stop"), this);
    m_stopButton->setMinimumHeight(34);
    connect(m_stopButton, &QPushButton::clicked, this, [this]() {
        if (m_recorder)
            m_recorder->stop();
    });
    buttonRow->addWidget(m_stopButton);

    m_refreshButton = new QPushButton(
        I18n::instance().text("recorder.button.refresh"), this);
    m_refreshButton->setMinimumHeight(34);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &RecorderPanel::refreshList);
    buttonRow->addWidget(m_refreshButton);

    buttonRow->addStretch(1);
    root->addLayout(buttonRow);

    // ---- 分隔线 ----
    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color:#D8DEE6;"));
    root->addWidget(sep);

    // ---- 录音列表 ----
    m_list = new QListWidget(this);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setAlternatingRowColors(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(m_list, &QListWidget::itemSelectionChanged,
            this, [this]() { updateControlsForState(
                                 m_recorder ? m_recorder->state()
                                            : RecorderState::Unavailable); });
    connect(m_list, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem *) { playSelected(); });
    root->addWidget(m_list, 1);

    // ---- 列表操作按钮 ----
    auto *listButtonRow = new QHBoxLayout;
    listButtonRow->setSpacing(8);

    m_playButton = new QPushButton(
        I18n::instance().text("recorder.button.play"), this);
    connect(m_playButton, &QPushButton::clicked,
            this, &RecorderPanel::playSelected);
    listButtonRow->addWidget(m_playButton);

    m_deleteButton = new QPushButton(
        I18n::instance().text("recorder.button.delete"), this);
    connect(m_deleteButton, &QPushButton::clicked, this, [this]() {
        // 默认走「移入回收」路径（软删除），可后悔
        deleteSelected(false);
    });
    listButtonRow->addWidget(m_deleteButton);

    m_purgeButton = new QPushButton(
        I18n::instance().text("recorder.button.purge"), this);
    m_purgeButton->setToolTip(I18n::instance().text("recorder.tooltip.purge"));
    connect(m_purgeButton, &QPushButton::clicked, this, [this]() {
        // 按保留期限清理旧文件 —— 这是"批量操作"，需要明确确认（SRS 4.3）
        if (!m_recorder)
            return;

        const int days = m_history ? m_history->privacySettings().retentionDays
                                   : 180;
        if (days <= 0) {
            QMessageBox::information(
                this, I18n::instance().text("recorder.title"),
                I18n::instance().text("recorder.purge.unlimited"));
            return;
        }

        if (!confirm(I18n::instance().text("recorder.purge.title"),
                     I18n::instance().text("recorder.purge.confirm",
                                           QStringLiteral("days"),
                                           QString::number(days)))) {
            return;
        }

        const int removed = m_recorder->purgeOlderThan(days);
        QMessageBox::information(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.purge.done",
                                  QStringLiteral("count"),
                                  QString::number(removed)));
        refreshList();
    });
    listButtonRow->addWidget(m_purgeButton);

    m_openFolderButton = new QPushButton(
        I18n::instance().text("recorder.button.open_folder"), this);
    connect(m_openFolderButton, &QPushButton::clicked,
            this, &RecorderPanel::openRecordingsFolder);
    listButtonRow->addWidget(m_openFolderButton);

    listButtonRow->addStretch(1);

    m_closeButton = new QPushButton(
        I18n::instance().text("recorder.button.close"), this);
    connect(m_closeButton, &QPushButton::clicked, this, &QDialog::hide);
    listButtonRow->addWidget(m_closeButton);

    root->addLayout(listButtonRow);
}

// ---------------------------------------------------------------------------
// 能力提示条
// ---------------------------------------------------------------------------
void RecorderPanel::updateCapabilityBanner()
{
    if (!m_bannerLabel)
        return;

    if (m_available) {
        // 可用时给一条"隐私承诺"性质的说明。
        //  这不是客套话：SRS 4.11 明确要求"不得未经许可上传"，
        //  把这条写在界面上，用户才有依据相信。
        m_bannerLabel->setText(
            I18n::instance().text("recorder.banner.local_only"));
        m_bannerLabel->setStyleSheet(QStringLiteral(
            "background:#E8F5E9; border:1px solid #A5D6A7;"
            "border-radius:6px; padding:8px 10px; color:#1B5E20;"));
        m_bannerLabel->setVisible(true);
        return;
    }

    // 不可用：必须写清原因（SRS 6.2）
    m_bannerLabel->setText(
        I18n::instance().text("recorder.banner.unavailable")
        + (m_unavailableReason.isEmpty()
               ? QString()
               : QStringLiteral(" ") + m_unavailableReason));
    m_bannerLabel->setStyleSheet(QStringLiteral(
        "background:#FFF8E1; border:1px solid #F0C36D;"
        "border-radius:6px; padding:8px 10px; color:#7A5A00;"));
    m_bannerLabel->setVisible(true);
}

bool RecorderPanel::isRecordingAvailable() const
{
    return m_available;
}

QString RecorderPanel::unavailableReason() const
{
    return m_unavailableReason;
}

// ---------------------------------------------------------------------------
// 打开面板
// ---------------------------------------------------------------------------
void RecorderPanel::open()
{
    // 每次打开都重新探测一次：用户可能刚刚插上/拔掉麦克风
    if (m_recorder) {
        QString reason;
        const bool ok = m_recorder->probeCapability(&reason);
        if (ok != m_available) {
            m_available = ok;
            m_unavailableReason = reason;
            DC_INFO(LogCat::Recording,
                    QStringLiteral("Recorder capability changed on open: %1")
                        .arg(ok ? QStringLiteral("available")
                                : QStringLiteral("unavailable")));
        } else if (!ok) {
            m_unavailableReason = reason;
        }
    }

    updateCapabilityBanner();
    updateControlsForState(m_recorder ? m_recorder->state()
                                      : RecorderState::Unavailable);
    refreshList();

    show();
    raise();
    activateWindow();
}

// ---------------------------------------------------------------------------
// 状态刷新
// ---------------------------------------------------------------------------
void RecorderPanel::updateControlsForState(RecorderState state)
{
    const bool recording = (state == RecorderState::Recording
                            || state == RecorderState::Starting);
    const bool busy = recording || state == RecorderState::Stopping;

    if (m_statusLabel) {
        m_statusLabel->setText(
            I18n::instance().text(
                QStringLiteral("recorder.state.%1")
                    .arg(recorderStateToString(state))));
    }

    // 呼吸点只在录音中显示 —— 它的存在本身就是"正在录"的信号
    if (m_dotLabel)
        m_dotLabel->setVisible(recording);

    if (m_recordButton) {
        m_recordButton->setEnabled(m_available
                                   && !busy
                                   && m_recorder != nullptr
                                   && m_recorder->isEnabled());
        if (!m_available) {
            m_recordButton->setToolTip(
                m_unavailableReason.isEmpty()
                    ? I18n::instance().text("recorder.disabled.generic")
                    : m_unavailableReason);
        } else if (m_recorder && !m_recorder->isEnabled()) {
            // 录音功能默认关闭（SRS 4.11），要在界面上说清怎么打开
            m_recordButton->setToolTip(
                I18n::instance().text("recorder.disabled.not_enabled"));
        } else {
            m_recordButton->setToolTip(
                I18n::instance().text("recorder.tooltip.start"));
        }
    }

    if (m_stopButton)
        m_stopButton->setEnabled(recording);

    const bool hasSelection = !selectedFilePath().isEmpty();
    if (m_playButton)
        m_playButton->setEnabled(hasSelection);
    if (m_deleteButton)
        m_deleteButton->setEnabled(hasSelection);
    if (m_purgeButton)
        m_purgeButton->setEnabled(m_recorder != nullptr);

    // 计时器颜色：录音中红、空闲灰
    if (m_timerLabel) {
        m_timerLabel->setStyleSheet(
            recording ? QStringLiteral("color:%1;").arg(kRecordingColor)
                      : QStringLiteral("color:#5A6472;"));
    }

    // 启动/停止呼吸动画
    if (recording && !m_blinkTimer) {
        m_blinkTimer = new QTimer(this);
        m_blinkTimer->setInterval(kBlinkIntervalMs);
        connect(m_blinkTimer, &QTimer::timeout, this, [this]() {
            if (!m_dotLabel)
                return;
            // 两相闪烁：足够引起注意，但不会像 1Hz 那样让人烦躁
            m_blinkPhase = (m_blinkPhase + 1) % 2;
            m_dotLabel->setStyleSheet(
                QStringLiteral("color:%1; font-size:15px;")
                    .arg(m_blinkPhase == 0 ? QString::fromLatin1(kRecordingColor)
                                           : QStringLiteral("#F2B8B8")));
        });
        m_blinkTimer->start();
    } else if (!recording && m_blinkTimer) {
        m_blinkTimer->stop();
        delete m_blinkTimer;
        m_blinkTimer = nullptr;
        if (m_dotLabel)
            m_dotLabel->setVisible(false);
    }

    // 文件夹路径提示
    if (m_folderLabel) {
        const QString dir = m_recorder ? m_recorder->outputDirectory() : QString();
        m_folderLabel->setText(
            dir.isEmpty()
                ? I18n::instance().text("recorder.folder.unknown")
                : I18n::instance().text("recorder.folder.hint",
                                        QStringLiteral("path"), dir));
    }

    // 存储占用
    if (m_recorder && m_storageBar && m_storageLabel) {
        const qint64 total = m_recorder->totalStorageBytes();
        const qint64 limit = m_recorder->config().maxTotalBytes;

        m_storageLabel->setText(
            I18n::instance().text("recorder.storage.usage",
                                  QStringLiteral("used"), formatBytes(total),
                                  QStringLiteral("limit"), formatBytes(limit)));

        const int pct = percentOf(total, limit);
        m_storageBar->setValue(pct);
        m_storageBar->setStyleSheet(
            pct >= 90
                ? QStringLiteral("QProgressBar::chunk{background:%1;}")
                      .arg(kRecordingColor)
                : QStringLiteral("QProgressBar::chunk{background:#5B9BD5;}"));
    }
}

// ---------------------------------------------------------------------------
// 计时 / 进度
// ---------------------------------------------------------------------------
void RecorderPanel::updateProgress(int durationMs, qint64 bytesWritten)
{
    m_lastDurationMs = durationMs;
    m_lastBytes = bytesWritten;

    if (m_timerLabel)
        m_timerLabel->setText(formatDuration(durationMs));

    if (m_durationBar && m_recorder) {
        const int maxSec = m_recorder->config().maxDurationSeconds;
        const int maxMs = qMax(1, maxSec * 1000);
        const int pct = percentOf(static_cast<qint64>(durationMs),
                                  static_cast<qint64>(maxMs));
        m_durationBar->setValue(pct);
        m_durationBar->setStyleSheet(
            pct >= 90
                ? QStringLiteral("QProgressBar::chunk{background:%1;}")
                      .arg(kRecordingColor)
                : QStringLiteral("QProgressBar::chunk{background:#5B9BD5;}"));
    }

    // 存储条也要跟着涨（录音在写文件）
    if (m_recorder && m_storageBar && m_storageLabel) {
        const qint64 total = m_recorder->totalStorageBytes();
        const qint64 limit = m_recorder->config().maxTotalBytes;
        const int pct = percentOf(total, limit);
        m_storageBar->setValue(pct);
        m_storageLabel->setText(
            I18n::instance().text("recorder.storage.usage",
                                  QStringLiteral("used"), formatBytes(total + bytesWritten),
                                  QStringLiteral("limit"), formatBytes(limit)));
    }
}

// ---------------------------------------------------------------------------
// 服务信号处理
// ---------------------------------------------------------------------------
void RecorderPanel::onRecorderStateChanged(RecorderState state)
{
    updateControlsForState(state);

    if (state == RecorderState::Idle) {
        // 回到空闲：把界面复位（但保留最后时长，用户会想看"刚录了多久"）
        if (m_durationBar)
            m_durationBar->setValue(0);
    }
}

void RecorderPanel::onRecordingStarted(const RecordingSession &session)
{
    m_lastBytes = 0;
    m_lastDurationMs = 0;

    if (m_timerLabel)
        m_timerLabel->setText(QStringLiteral("00:00"));

    DC_INFO(LogCat::Recording,
            QStringLiteral("Recording UI: session started"));

    updateControlsForState(RecorderState::Recording);

    Q_UNUSED(session);
}

void RecorderPanel::onRecordingStopped(const RecordingSession &session)
{
    updateControlsForState(m_recorder ? m_recorder->state()
                                      : RecorderState::Idle);

    // 自动停止要说清楚为什么（SRS 4.11：超限应告知，而不是默默截断）
    if (session.autoStopped && !session.stopReason.isEmpty()) {
        QMessageBox::information(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.auto_stopped",
                                  QStringLiteral("reason"), session.stopReason));
    }

    // 录完立刻刷新列表，让新文件出现在列表顶部
    refreshList();

    DC_INFO(LogCat::Recording,
            QStringLiteral("Recording UI: session stopped, duration=%1ms")
                .arg(session.durationMs));
}

void RecorderPanel::onRecordingProgress(int durationMs, qint64 bytesWritten)
{
    updateProgress(durationMs, bytesWritten);
}

void RecorderPanel::onLimitReached(const QString &reason)
{
    DC_WARN(LogCat::Recording,
            QStringLiteral("Recording limit reached: %1").arg(reason));

    // 托盘通知 + 这里也要说一声。
    //  为什么不在这里弹模态框：录音可能正在用户专注别的事情时结束，
    //  弹窗会打断他。托盘/气泡是更合适的渠道。
    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(
            QStringLiteral("color:%1; font-weight:600;").arg(kRecordingColor));
        m_statusLabel->setText(
            I18n::instance().text("recorder.limit.reached",
                                  QStringLiteral("reason"), reason));
    }
}

void RecorderPanel::onRecorderError(const QString &message)
{
    DC_ERROR(LogCat::Recording,
             QStringLiteral("Recorder error reported to UI: %1").arg(message));

    QMessageBox::warning(this,
                         I18n::instance().text("recorder.title"),
                         message.isEmpty()
                             ? I18n::instance().text("recorder.error.generic")
                             : message);
}

// ---------------------------------------------------------------------------
// 列表
// ---------------------------------------------------------------------------
void RecorderPanel::refreshList()
{
    if (!m_list)
        return;

    m_list->clear();

    if (!m_recorder) {
        updateControlsForState(RecorderState::Unavailable);
        return;
    }

    const QStringList files = m_recorder->listRecordings();

    for (const QString &path : files) {
        const QFileInfo info(path);

        auto *item = new QListWidgetItem(m_list);
        item->setText(QStringLiteral("%1    (%2)")
                          .arg(info.fileName(),
                               formatBytes(info.size())));
        // 完整路径放 tooltip / data：列表只显示文件名，但用户要看得到全路径
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);
    }

    if (files.isEmpty()) {
        // 空列表给一句提示，而不是一片空白让人以为坏了
        auto *item = new QListWidgetItem(m_list);
        item->setText(I18n::instance().text("recorder.list.empty"));
        item->setFlags(Qt::NoItemFlags);   // 不可选中
        item->setForeground(QColor(0x9A, 0xA4, 0xB0));
    }

    updateControlsForState(m_recorder->state());
}

int RecorderPanel::itemCount() const
{
    if (!m_list)
        return 0;

    // 空提示占位项不算有效条目
    int count = 0;
    for (int i = 0; i < m_list->count(); ++i) {
        const QListWidgetItem *item = m_list->item(i);
        if (item && !item->data(Qt::UserRole).toString().isEmpty())
            ++count;
    }
    return count;
}

QString RecorderPanel::selectedFilePath() const
{
    if (!m_list)
        return QString();

    const QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return QString();

    return item->data(Qt::UserRole).toString();
}

// ---------------------------------------------------------------------------
// 播放
//
//  用系统默认播放器打开，而不是内嵌播放。
//  理由：内嵌播放需要 Qt Multimedia，而我们的构建里它是可选的
//  （未安装时录音走降级路径）。如果播放依赖它，就会出现
//  "能录但不能播"的怪状态。交给系统播放器则永远可用。
// ---------------------------------------------------------------------------
void RecorderPanel::playSelected()
{
    const QString path = selectedFilePath();
    if (path.isEmpty())
        return;

    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.error.file_missing"));
        refreshList();
        return;
    }

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.error.play_failed"));
    }
}

// ---------------------------------------------------------------------------
// 删除
// ---------------------------------------------------------------------------
void RecorderPanel::deleteSelected(bool permanent)
{
    const QString path = selectedFilePath();
    if (path.isEmpty() || !m_recorder)
        return;

    const QFileInfo info(path);

    if (permanent) {
        // 永久删除：措辞必须说明"无法恢复"
        if (!confirm(I18n::instance().text("recorder.delete.permanent_title"),
                     I18n::instance().text("recorder.delete.permanent_confirm",
                                           QStringLiteral("name"), info.fileName()))) {
            return;
        }
    } else {
        // 普通删除：也说清楚会删文件（因为这个实现是真的删文件，
        //  不是移到系统回收站 —— 不能给用户错误预期）
        if (!confirm(I18n::instance().text("recorder.delete.title"),
                     I18n::instance().text("recorder.delete.confirm",
                                           QStringLiteral("name"), info.fileName()))) {
            return;
        }
    }

    const RecorderResult r = m_recorder->deleteRecording(path);
    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("recorder.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("recorder.error.delete_failed")
                                 : r.message);
        return;
    }

    DC_INFO(LogCat::Recording,
            QStringLiteral("Recording deleted by user (permanent=%1)")
                .arg(permanent ? QStringLiteral("yes") : QStringLiteral("no")));

    refreshList();
}

// ---------------------------------------------------------------------------
// 打开文件夹
// ---------------------------------------------------------------------------
void RecorderPanel::openRecordingsFolder()
{
    if (!m_recorder)
        return;

    const QString dir = m_recorder->outputDirectory();
    if (dir.isEmpty()) {
        QMessageBox::information(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.folder.unknown"));
        return;
    }

    // 目录可能还不存在（一次都没录过），先建出来再打开，
    //  否则系统会打开一个"找不到"的提示，体验很差。
    QDir().mkpath(dir);

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) {
        QMessageBox::information(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.folder.open_failed",
                                  QStringLiteral("path"), dir));
    }
}

// ---------------------------------------------------------------------------
// 确认框
// ---------------------------------------------------------------------------
bool RecorderPanel::confirm(const QString &title, const QString &text)
{
    const QMessageBox::StandardButton answer =
        QMessageBox::question(this, title, text,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);   // 默认"否"
    return answer == QMessageBox::Yes;
}

// ---------------------------------------------------------------------------
// 格式化
// ---------------------------------------------------------------------------
QString RecorderPanel::formatDuration(int durationMs)
{
    if (durationMs < 0)
        durationMs = 0;

    const int totalSeconds = durationMs / 1000;
    const int minutes = totalSeconds / 60;
    const int seconds = totalSeconds % 60;

    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QString RecorderPanel::formatBytes(qint64 bytes)
{
    if (bytes < 0)
        bytes = 0;

    constexpr qint64 kKiB = 1024;
    constexpr qint64 kMiB = 1024 * kKiB;
    constexpr qint64 kGiB = 1024 * kMiB;

    if (bytes >= kGiB)
        return QStringLiteral("%1 GB").arg(double(bytes) / kGiB, 0, 'f', 2);
    if (bytes >= kMiB)
        return QStringLiteral("%1 MB").arg(double(bytes) / kMiB, 0, 'f', 1);
    if (bytes >= kKiB)
        return QStringLiteral("%1 KB").arg(double(bytes) / kKiB, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

// ---------------------------------------------------------------------------
// 关闭与键盘
// ---------------------------------------------------------------------------
void RecorderPanel::closeEvent(QCloseEvent *event)
{
    // 录音中关面板：不阻止关闭（面板只是界面），但提醒一下还在录。
    //  真正的停止由用户显式操作或托盘控制 —— 界面关闭不等于停止录音，
    //  这一点必须让用户知道，否则他会以为关掉就停了。
    if (m_recorder && m_recorder->isRecording()) {
        const auto answer = QMessageBox::question(
            this, I18n::instance().text("recorder.title"),
            I18n::instance().text("recorder.close.while_recording"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);

        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
    }

    event->accept();
    // 不 delete，复用实例；真正的析构在 main.cpp 退出时
    hide();
}

void RecorderPanel::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        // Esc = 隐藏面板（SRS 4.12：Esc 关闭当前弹窗）
        hide();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace dc
