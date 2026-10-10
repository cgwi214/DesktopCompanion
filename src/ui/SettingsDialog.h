// ============================================================================
//  SettingsDialog.h — 设置窗口
//
//  需求依据：SRS V1.1 第 5 节（配置设计）+ 各功能节的"可配置项"
//    - 配置以可读文件形式保存，用户可手工编辑
//    - 配置项变更后应明确告知生效方式
//    - 不安全的开关不得提供（或在保存时强制回正）
//    - 支持导入/导出配置，导出默认不包含密钥
//    - 提供恢复默认
//
//  关键设计决定：
//
//    1. 【本窗口不直接写配置文件】
//       所有改动先落在内存里的 AppConfig，只有用户点"保存"时才通过
//       CoreRuntime::saveConfig() 落盘。
//       为什么不让每个控件一变就写盘：
//          a. 用户改到一半关掉窗口，不该留下半个配置
//          b. 频繁写盘会磨损 + 在旧硬盘上明显卡顿
//          c. "我改了什么"这件事需要一个明确的确认点
//       同时为了不骗用户，窗口底部常驻一行"有未保存的改动"。
//
//    2. 【不安全开关不存在，而不是"存在但保存时改回"】
//       SRS 5 规定若干开关恒为安全值（如允许不安全 TLS、记录完整路径、
//       遥测上报）。这些我们**根本不生成控件**。
//       如果只在保存时强制回正，用户会以为设置生效了 —— 那更糟。
//       所以界面上会看到一句灰字说明"该项出于安全考虑不可更改"。
//
//    3. 【能力相关项按探测结果禁用并解释原因】
//       例如 Qt Multimedia 未编译进来时，录音页的开关要灰掉，
//       并写明"当前构建未包含音频模块"（SRS 6.2）。
//
//    4. 【"关于/诊断"页是排障入口，不是装饰】
//       它展示：路径布局（数据存哪里）、能力报告、启动降级说明、
//       环境隔离位置清单。用户报告问题时可以直接截图这一页。
//
//    5. 【热键录入用 QKeySequenceEdit】
//       手打 "Ctrl+Alt+L" 极易拼错且平台差异大。
//       用录入控件让用户按键，程序负责格式化。
// ============================================================================
#pragma once

#include "core/ConfigManager.h"
#include "model/Types.h"
#include "platform/SystemInfo.h"

#include <QDialog>
#include <QString>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTextBrowser;
QT_END_NAMESPACE

namespace dc {

class CoreRuntime;
class SpeechService;
class RecorderService;

// ---------------------------------------------------------------------------
// SettingsDialog
//
//  构造时只拿走 CoreRuntime 指针。所有服务通过它按需取用 ——
//  这样即使某个服务为 nullptr（降级启动），本窗口也能正常显示，
//  只需把相关分页禁用即可。
// ---------------------------------------------------------------------------
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(CoreRuntime *runtime, QWidget *parent = nullptr);
    ~SettingsDialog() override;

    // 打开窗口：从当前配置重建界面后显示
    void open();

    // 是否有未保存的改动
    bool hasUnsavedChanges() const { return m_dirty; }

    // 把界面上的值写回配置对象（不落盘）
    void collectIntoConfig(AppConfig *config) const;

    // 把配置对象灌进界面控件（不读盘）
    void applyConfigToUi(const AppConfig &config);

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onSave();
    void onResetToDefaults();
    void onImport();
    void onExport();
    void onOpenDataDirectory();
    void onOpenLogDirectory();
    void onReloadRules();

private:
    // 建各分页
    void buildUi();
    QWidget *buildGeneralTab();
    QWidget *buildAppearanceTab();
    QWidget *buildCommandTab();
    QWidget *buildNetworkTab();
    QWidget *buildSpeechTab();
    QWidget *buildHistoryTab();
    QWidget *buildHotkeyTab();
    QWidget *buildDiagnosticsTab();

    // 触发"有未保存改动"
    void markDirty();
    // 刷新底部状态行
    void updateDirtyIndicator();

    // 从 runtime 当前配置重建界面（下拉框选项等）
    void rebuildFromRuntime();

    // 能力/限制造说明文字
    QString speechLimitationText() const;
    QString recorderLimitationText() const;

    // 诊断页内容
    QString buildDiagnosticsHtml() const;

    // 保存前校验；返回 false 表示有阻断性问题
    bool validateBeforeSave(QString *message) const;

    CoreRuntime *m_runtime = nullptr;

    AppConfig m_draft;     // 编辑中的配置（保存前不落盘）
    bool m_dirty = false;
    bool m_loading = false;   // 灌数据期间不要误报 dirty

    QTabWidget *m_tabs = nullptr;

    // ---- 通用 ----
    QComboBox *m_languageCombo = nullptr;
    QCheckBox *m_startupEnabled = nullptr;
    QCheckBox *m_startupMinimized = nullptr;
    QSpinBox *m_startupDelay = nullptr;
    QLabel *m_startupAvailability = nullptr;

    // ---- 外观 ----
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityValue = nullptr;
    QSlider *m_scaleSlider = nullptr;
    QLabel *m_scaleValue = nullptr;
    QCheckBox *m_alwaysOnTop = nullptr;
    QCheckBox *m_startHidden = nullptr;
    QCheckBox *m_trayIconEnabled = nullptr;
    QCheckBox *m_showStatusHints = nullptr;
    QSpinBox *m_bubbleDuration = nullptr;
    QSpinBox *m_bubbleMaxWidth = nullptr;
    QSpinBox *m_animationInterval = nullptr;
    QLineEdit *m_petImagePath = nullptr;
    QPushButton *m_pickPetImage = nullptr;

    // ---- 命令 ----
    QDoubleSpinBox *m_minConfidence = nullptr;
    QSpinBox *m_maxInputLength = nullptr;
    QCheckBox *m_allowUserRules = nullptr;
    QCheckBox *m_confirmMediumRisk = nullptr;
    QCheckBox *m_authorizeHighRisk = nullptr;
    QCheckBox *m_scriptsEnabled = nullptr;
    QListWidget *m_ruleFileList = nullptr;

    // ---- 网络 ----
    QComboBox *m_networkMode = nullptr;
    QCheckBox *m_askRemember = nullptr;
    QSpinBox *m_connectTimeout = nullptr;
    QSpinBox *m_readTimeout = nullptr;
    QSpinBox *m_maxRetries = nullptr;
    QLabel *m_insecureTlsNote = nullptr;

    // ---- 语音与录音 ----
    QCheckBox *m_speechEnabled = nullptr;
    QComboBox *m_recognitionLanguage = nullptr;
    QCheckBox *m_pushToTalk = nullptr;
    QCheckBox *m_synthesisEnabled = nullptr;
    QSlider *m_speechRate = nullptr;
    QLabel *m_speechRateValue = nullptr;
    QSlider *m_speechVolume = nullptr;
    QLabel *m_speechVolumeValue = nullptr;
    QLabel *m_speechLimitation = nullptr;

    QCheckBox *m_recordingEnabled = nullptr;
    QSpinBox *m_recordingMaxDuration = nullptr;
    QComboBox *m_recordingSampleRate = nullptr;
    QLabel *m_recordingLimitation = nullptr;

    // ---- 历史与隐私 ----
    QCheckBox *m_historyConversation = nullptr;
    QCheckBox *m_historyTask = nullptr;
    QCheckBox *m_historyTranslation = nullptr;
    QCheckBox *m_historySearch = nullptr;
    QCheckBox *m_historyRecording = nullptr;
    QSpinBox *m_retentionDays = nullptr;
    QSpinBox *m_maxStorageMb = nullptr;
    QCheckBox *m_auditLogEnabled = nullptr;
    QLabel *m_fixedPrivacyNote = nullptr;
    QPushButton *m_clearHistoryButton = nullptr;

    // ---- 快捷键 ----
    QCheckBox *m_hotkeyEnabled = nullptr;
    QKeySequenceEdit *m_hotkeyTogglePet = nullptr;
    QKeySequenceEdit *m_hotkeyToggleMic = nullptr;
    QKeySequenceEdit *m_hotkeyOpenHistory = nullptr;
    QKeySequenceEdit *m_hotkeyOpenInput = nullptr;
    QLabel *m_hotkeyAvailability = nullptr;
    QLabel *m_hotkeyConflicts = nullptr;

    // ---- 诊断 ----
    QTextBrowser *m_diagnosticsView = nullptr;

    // ---- 底部 ----
    QLabel *m_dirtyLabel = nullptr;
    QPushButton *m_saveButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_closeButton = nullptr;
};

} // namespace dc
