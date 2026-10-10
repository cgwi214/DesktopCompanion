// ============================================================================
//  RecorderPanel.h — 录音面板
//
//  需求依据：SRS V1.1 第 4.11 节（录音功能）
//    - 录音文件默认保存在本地
//    - 录音仅在用户显式触发时进行；默认关闭
//    - 进行中须有明显提示（不能"偷偷录"）
//    - 录音有最长时长与最大文件大小限制
//    - 提供录音列表：播放、删除、按时间清理
//    - 不得在未经用户明确许可时上传录音
//
//  关键设计决定：
//
//    1. 【面板一打开就告诉用户"能不能录"】
//       SRS 6.2 要求能力不确定/不可用时明确说明。
//       所以 initialize() 会先调 RecorderService::probeCapability()，
//       不可用就在……面板顶部放一条醒目的说明，
//       并把录音按钮禁用 —— 而不是让用户按下去之后才报错。
//
//    2. 【录音中必须在界面上持续可见】
//       SRS 4.11 要求"进行中须有明显提示"。
//       所以录音时：
//         - 面板显示醒目的红色计时器 + 呼吸圆点
//         - 托盘图标切到 Listening 状态
//         - 桌宠切到 Listening 状态
//       三个地方同时提示，用户不可能不知道正在录音。
//       这也是隐私保护的实质部分 —— 用户不该需要"回想一下"是否在录。
//
//    3. 【删除录音要二次确认，且进回收逻辑（软删除）】
//       录音是不可再生的用户数据。虽然 RecorderService 提供
//       deleteRecording()（真的删文件），但界面默认走"标记删除"，
//       真正删文件需要再点一次"永久删除"。
//       理由：误删一段会议录音的代价远大于多确认一次的麻烦。
//
//    4. 【不提供"上传"按钮】
//       SRS 4.11 明确禁止未经许可上传录音。
//       最彻底的合规方式是界面上根本没有这个入口 ——
//       没有入口，就不可能有"误点上传"。
//       如果你后面要加分享功能，请先读这一条。
//
//    5. 【文件大小/时长上限显示为进度条】
//       到达上限会自动停止（RecorderService 负责），但用户应该
//       提前看到"还有多久到顶"，而不是突然被打断。
// ============================================================================
#pragma once

#include "services/RecorderService.h"

#include <QDialog>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace dc {

class HistoryService;

// ---------------------------------------------------------------------------
// RecorderPanel
// ---------------------------------------------------------------------------
class RecorderPanel : public QDialog
{
    Q_OBJECT

public:
    // -----------------------------------------------------------------------
    //  recorder  —— 录音服务（允许为 nullptr：此时面板整体显示为"不可用"）
    //  history   —— 用于刷新录音元数据列表（允许为 nullptr）
    // -----------------------------------------------------------------------
    explicit RecorderPanel(RecorderService *recorder,
                           HistoryService *history = nullptr,
                           QWidget *parent = nullptr);
    ~RecorderPanel() override;

    // 打开面板：先探测能力、刷新列表，再显示
    void open();

    // 能力探测结果（供测试）
    bool isRecordingAvailable() const;
    QString unavailableReason() const;

    // 刷新录音文件列表
    void refreshList();

    // 当前列表条目数（供测试）
    int itemCount() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    // ---- 服务信号 ----
    void onRecorderStateChanged(dc::RecorderState state);
    void onRecordingStarted(const dc::RecordingSession &session);
    void onRecordingStopped(const dc::RecordingSession &session);
    void onRecordingProgress(int durationMs, qint64 bytesWritten);
    void onLimitReached(const QString &reason);
    void onRecorderError(const QString &message);

private:
    // 建界面
    void buildUi();

    // 顶部能力提示条
    void updateCapabilityBanner();

    // 按当前状态刷新按钮/计时器/进度条
    void updateControlsForState(RecorderState state);

    // 刷新计时与容量进度
    void updateProgress(int durationMs, qint64 bytesWritten);

    // 格式化为 mm:ss
    static QString formatDuration(int durationMs);
    // 格式化为人类可读大小
    static QString formatBytes(qint64 bytes);

    // 录音列表项的操作
    void playSelected();
    void deleteSelected(bool permanent);
    void openRecordingsFolder();

    // 当前选中的文件路径（无选中返回空）
    QString selectedFilePath() const;

    // 二次确认对话框（统一措辞）
    bool confirm(const QString &title, const QString &text);

    RecorderService *m_recorder = nullptr;
    HistoryService *m_history = nullptr;

    bool m_available = false;
    QString m_unavailableReason;

    // ---- 控件 ----
    QLabel *m_bannerLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_timerLabel = nullptr;
    QLabel *m_dotLabel = nullptr;             // 呼吸圆点（用文字 ● 做，简单可靠）
    QProgressBar *m_durationBar = nullptr;
    QProgressBar *m_storageBar = nullptr;
    QLabel *m_storageLabel = nullptr;
    QLabel *m_folderLabel = nullptr;

    QPushButton *m_recordButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPushButton *m_purgeButton = nullptr;
    QPushButton *m_openFolderButton = nullptr;
    QPushButton *m_closeButton = nullptr;

    QListWidget *m_list = nullptr;

    // 呼吸点动画
    QTimer *m_blinkTimer = nullptr;
    int m_blinkPhase = 0;

    // 上次进度（用于停止后显示最终值）
    int m_lastDurationMs = 0;
    qint64 m_lastBytes = 0;
};

} // namespace dc
