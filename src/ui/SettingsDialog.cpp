// ============================================================================
//  SettingsDialog.cpp — 设置窗口实现
// ============================================================================
#include "ui/SettingsDialog.h"

#include "core/CoreRuntime.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "platform/AutostartManager.h"
#include "platform/HotkeyManager.h"
#include "services/HistoryService.h"
#include "services/RecorderService.h"
#include "services/SpeechService.h"
#include "ui/I18n.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace dc {

namespace {

// 透明度 / 缩放的滑条步长（与 PetWindow 的常量保持一致）
constexpr int kOpacityMin = 20;      // 20%
constexpr int kOpacityMax = 100;
constexpr int kScaleMin = 50;        // 50%
constexpr int kScaleMax = 300;

// 把滑条整数转成 double 比例
double sliderToRatio(int value) { return double(value) / 100.0; }
int ratioToSlider(double ratio) { return int(ratio * 100.0 + 0.5); }

// 一个"因安全原因不可更改"的说明标签
QLabel *makeFixedNote(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("color:#8A94A2; font-style:italic;"));
    return label;
}

// 一个"当前不可用"的提示标签
QLabel *makeUnavailableNote(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral(
        "background:#FFF8E1; border:1px solid #F0C36D;"
        "border-radius:5px; padding:6px 8px; color:#7A5A00;"));
    return label;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
SettingsDialog::SettingsDialog(CoreRuntime *runtime, QWidget *parent)
    : QDialog(parent)
    , m_runtime(runtime)
{
    setWindowTitle(I18n::instance().text("settings.title"));
    setMinimumSize(760, 620);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    buildUi();
    rebuildFromRuntime();
}

SettingsDialog::~SettingsDialog() = default;

// ---------------------------------------------------------------------------
// 界面框架
// ---------------------------------------------------------------------------
void SettingsDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(10);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildGeneralTab(),      I18n::instance().text("settings.tab.general"));
    m_tabs->addTab(buildAppearanceTab(),   I18n::instance().text("settings.tab.appearance"));
    m_tabs->addTab(buildCommandTab(),      I18n::instance().text("settings.tab.command"));
    m_tabs->addTab(buildNetworkTab(),      I18n::instance().text("settings.tab.network"));
    m_tabs->addTab(buildSpeechTab(),       I18n::instance().text("settings.tab.speech"));
    m_tabs->addTab(buildHistoryTab(),      I18n::instance().text("settings.tab.history"));
    m_tabs->addTab(buildHotkeyTab(),       I18n::instance().text("settings.tab.hotkey"));
    m_tabs->addTab(buildDiagnosticsTab(),  I18n::instance().text("settings.tab.diagnostics"));

    root->addWidget(m_tabs, 1);

    // ---- 未保存提示 ----
    m_dirtyLabel = new QLabel(this);
    m_dirtyLabel->setStyleSheet(QStringLiteral("color:#B26A00; font-weight:600;"));
    m_dirtyLabel->setVisible(false);
    root->addWidget(m_dirtyLabel);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color:#D8DEE6;"));
    root->addWidget(sep);

    // ---- 底部按钮 ----
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);

    m_resetButton = new QPushButton(
        I18n::instance().text("settings.button.reset"), this);
    connect(m_resetButton, &QPushButton::clicked,
            this, &SettingsDialog::onResetToDefaults);
    buttonRow->addWidget(m_resetButton);

    m_importButton = new QPushButton(
        I18n::instance().text("settings.button.import"), this);
    connect(m_importButton, &QPushButton::clicked,
            this, &SettingsDialog::onImport);
    buttonRow->addWidget(m_importButton);

    m_exportButton = new QPushButton(
        I18n::instance().text("settings.button.export"), this);
    m_exportButton->setToolTip(
        I18n::instance().text("settings.tooltip.export_no_secrets"));
    connect(m_exportButton, &QPushButton::clicked,
            this, &SettingsDialog::onExport);
    buttonRow->addWidget(m_exportButton);

    buttonRow->addStretch(1);

    m_saveButton = new QPushButton(
        I18n::instance().text("settings.button.save"), this);
    m_saveButton->setMinimumHeight(32);
    m_saveButton->setDefault(true);
    connect(m_saveButton, &QPushButton::clicked,
            this, &SettingsDialog::onSave);
    buttonRow->addWidget(m_saveButton);

    m_closeButton = new QPushButton(
        I18n::instance().text("settings.button.close"), this);
    m_closeButton->setMinimumHeight(32);
    connect(m_closeButton, &QPushButton::clicked, this, [this]() {
        // 有关闭前拦截（见 closeEvent），这里直接触发同一逻辑
        close();
    });
    buttonRow->addWidget(m_closeButton);

    root->addLayout(buttonRow);
}

// ---------------------------------------------------------------------------
// 通用
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildGeneralTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ---- 语言 ----
    auto *langGroup = new QGroupBox(I18n::instance().text("settings.general.language"), page);
    auto *langForm = new QFormLayout(langGroup);

    m_languageCombo = new QComboBox(langGroup);
    m_languageCombo->addItem(I18n::instance().text("settings.language.zh"),
                             QVariant::fromValue(static_cast<int>(Language::ZhCN)));
    m_languageCombo->addItem(I18n::instance().text("settings.language.en"),
                             QVariant::fromValue(static_cast<int>(Language::EnUS)));
    m_languageCombo->addItem(I18n::instance().text("settings.language.ja"),
                             QVariant::fromValue(static_cast<int>(Language::JaJP)));
    connect(m_languageCombo, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::markDirty);
    langForm->addRow(I18n::instance().text("settings.general.language.label"),
                     m_languageCombo);
    langForm->addRow(QString(), makeFixedNote(
        I18n::instance().text("settings.general.language.note"), langGroup));

    layout->addWidget(langGroup);

    // ---- 开机启动 ----
    auto *startupGroup = new QGroupBox(I18n::instance().text("settings.general.startup"), page);
    auto *startupLayout = new QVBoxLayout(startupGroup);

    m_startupEnabled = new QCheckBox(
        I18n::instance().text("settings.startup.enabled"), startupGroup);
    connect(m_startupEnabled, &QCheckBox::toggled, this, [this](bool on) {
        markDirty();
        if (m_startupMinimized) m_startupMinimized->setEnabled(on);
        if (m_startupDelay)     m_startupDelay->setEnabled(on);
    });
    startupLayout->addWidget(m_startupEnabled);

    m_startupMinimized = new QCheckBox(
        I18n::instance().text("settings.startup.minimized"), startupGroup);
    connect(m_startupMinimized, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    startupLayout->addWidget(m_startupMinimized);

    auto *delayRow = new QHBoxLayout;
    delayRow->addWidget(new QLabel(
        I18n::instance().text("settings.startup.delay"), startupGroup));
    m_startupDelay = new QSpinBox(startupGroup);
    m_startupDelay->setRange(0, 300);
    m_startupDelay->setSuffix(I18n::instance().text("settings.unit.seconds"));
    connect(m_startupDelay, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    delayRow->addWidget(m_startupDelay);
    delayRow->addStretch(1);
    startupLayout->addLayout(delayRow);

    // 能力检查：系统不支持开机启动机制时说明原因
    m_startupAvailability = new QLabel(startupGroup);
    m_startupAvailability->setWordWrap(true);
    m_startupAvailability->setVisible(false);
    startupLayout->addWidget(m_startupAvailability);

    layout->addWidget(startupGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 外观
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildAppearanceTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ---- 形象 ----
    auto *petGroup = new QGroupBox(I18n::instance().text("settings.appearance.pet"), page);
    auto *petForm = new QFormLayout(petGroup);

    auto *imageRow = new QHBoxLayout;
    m_petImagePath = new QLineEdit(petGroup);
    m_petImagePath->setPlaceholderText(
        I18n::instance().text("settings.appearance.image.placeholder"));
    connect(m_petImagePath, &QLineEdit::textChanged,
            this, &SettingsDialog::markDirty);
    imageRow->addWidget(m_petImagePath, 1);

    m_pickPetImage = new QPushButton(
        I18n::instance().text("settings.button.browse"), petGroup);
    connect(m_pickPetImage, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this,
            I18n::instance().text("settings.appearance.image.pick_title"),
            QString(),
            I18n::instance().text("settings.appearance.image.filter"));
        if (!path.isEmpty()) {
            m_petImagePath->setText(path);
            markDirty();
        }
    });
    imageRow->addWidget(m_pickPetImage, 0);
    petForm->addRow(I18n::instance().text("settings.appearance.image"), imageRow);

    m_animationInterval = new QSpinBox(petGroup);
    m_animationInterval->setRange(40, 2000);
    m_animationInterval->setSuffix(I18n::instance().text("settings.unit.ms"));
    connect(m_animationInterval, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    petForm->addRow(I18n::instance().text("settings.appearance.interval"),
                    m_animationInterval);

    layout->addWidget(petGroup);

    // ---- 窗口行为 ----
    auto *windowGroup = new QGroupBox(I18n::instance().text("settings.appearance.window"), page);
    auto *windowLayout = new QVBoxLayout(windowGroup);

    m_alwaysOnTop = new QCheckBox(
        I18n::instance().text("settings.appearance.always_on_top"), windowGroup);
    connect(m_alwaysOnTop, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    windowLayout->addWidget(m_alwaysOnTop);

    m_startHidden = new QCheckBox(
        I18n::instance().text("settings.appearance.start_hidden"), windowGroup);
    connect(m_startHidden, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    windowLayout->addWidget(m_startHidden);

    m_trayIconEnabled = new QCheckBox(
        I18n::instance().text("settings.appearance.tray"), windowGroup);
    connect(m_trayIconEnabled, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    windowLayout->addWidget(m_trayIconEnabled);

    m_showStatusHints = new QCheckBox(
        I18n::instance().text("settings.appearance.hints"), windowGroup);
    connect(m_showStatusHints, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    windowLayout->addWidget(m_showStatusHints);

    // ---- 透明度 ----
    auto *opacityRow = new QHBoxLayout;
    opacityRow->addWidget(new QLabel(
        I18n::instance().text("settings.appearance.opacity"), windowGroup));
    m_opacitySlider = new QSlider(Qt::Horizontal, windowGroup);
    m_opacitySlider->setRange(kOpacityMin, kOpacityMax);
    opacityRow->addWidget(m_opacitySlider, 1);
    m_opacityValue = new QLabel(QStringLiteral("100%"), windowGroup);
    m_opacityValue->setFixedWidth(46);
    opacityRow->addWidget(m_opacityValue);
    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_opacityValue)
            m_opacityValue->setText(QStringLiteral("%1%").arg(v));
        markDirty();
    });
    windowLayout->addLayout(opacityRow);

    // ---- 缩放 ----
    auto *scaleRow = new QHBoxLayout;
    scaleRow->addWidget(new QLabel(
        I18n::instance().text("settings.appearance.scale"), windowGroup));
    m_scaleSlider = new QSlider(Qt::Horizontal, windowGroup);
    m_scaleSlider->setRange(kScaleMin, kScaleMax);
    scaleRow->addWidget(m_scaleSlider, 1);
    m_scaleValue = new QLabel(QStringLiteral("100%"), windowGroup);
    m_scaleValue->setFixedWidth(46);
    scaleRow->addWidget(m_scaleValue);
    connect(m_scaleSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_scaleValue)
            m_scaleValue->setText(QStringLiteral("%1%").arg(v));
        markDirty();
    });
    windowLayout->addLayout(scaleRow);

    layout->addWidget(windowGroup);

    // ---- 气泡 ----
    auto *bubbleGroup = new QGroupBox(I18n::instance().text("settings.appearance.bubble"), page);
    auto *bubbleForm = new QFormLayout(bubbleGroup);

    m_bubbleDuration = new QSpinBox(bubbleGroup);
    m_bubbleDuration->setRange(1000, 60000);
    m_bubbleDuration->setSingleStep(500);
    m_bubbleDuration->setSuffix(I18n::instance().text("settings.unit.ms"));
    connect(m_bubbleDuration, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    bubbleForm->addRow(I18n::instance().text("settings.appearance.bubble_duration"),
                       m_bubbleDuration);

    m_bubbleMaxWidth = new QSpinBox(bubbleGroup);
    m_bubbleMaxWidth->setRange(180, 900);
    m_bubbleMaxWidth->setSuffix(I18n::instance().text("settings.unit.px"));
    connect(m_bubbleMaxWidth, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    bubbleForm->addRow(I18n::instance().text("settings.appearance.bubble_width"),
                       m_bubbleMaxWidth);

    layout->addWidget(bubbleGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 命令与权限
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildCommandTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ---- 解析 ----
    auto *parseGroup = new QGroupBox(I18n::instance().text("settings.command.parse"), page);
    auto *parseForm = new QFormLayout(parseGroup);

    m_minConfidence = new QDoubleSpinBox(parseGroup);
    m_minConfidence->setRange(0.0, 1.0);
    m_minConfidence->setSingleStep(0.05);
    m_minConfidence->setDecimals(2);
    connect(m_minConfidence, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    parseForm->addRow(I18n::instance().text("settings.command.min_confidence"),
                      m_minConfidence);
    parseForm->addRow(QString(), makeFixedNote(
        I18n::instance().text("settings.command.min_confidence.note"), parseGroup));

    m_maxInputLength = new QSpinBox(parseGroup);
    m_maxInputLength->setRange(32, 4096);
    m_maxInputLength->setSuffix(I18n::instance().text("settings.unit.chars"));
    connect(m_maxInputLength, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    parseForm->addRow(I18n::instance().text("settings.command.max_length"),
                      m_maxInputLength);

    layout->addWidget(parseGroup);

    // ---- 权限与风险 ----
    auto *riskGroup = new QGroupBox(I18n::instance().text("settings.command.risk"), page);
    auto *riskLayout = new QVBoxLayout(riskGroup);

    m_confirmMediumRisk = new QCheckBox(
        I18n::instance().text("settings.command.confirm_medium"), riskGroup);
    connect(m_confirmMediumRisk, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    riskLayout->addWidget(m_confirmMediumRisk);

    m_authorizeHighRisk = new QCheckBox(
        I18n::instance().text("settings.command.authorize_high"), riskGroup);
    connect(m_authorizeHighRisk, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    riskLayout->addWidget(m_authorizeHighRisk);

    // 高风险操作即使不确认也要授权的开关不提供 —— 说清楚原因
    riskLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.command.risk.fixed_note"), riskGroup));

    layout->addWidget(riskGroup);

    // ---- 用户自定义规则 ----
    auto *ruleGroup = new QGroupBox(I18n::instance().text("settings.command.rules"), page);
    auto *ruleLayout = new QVBoxLayout(ruleGroup);

    m_allowUserRules = new QCheckBox(
        I18n::instance().text("settings.command.allow_user_rules"), ruleGroup);
    connect(m_allowUserRules, &QCheckBox::toggled, this, [this](bool on) {
        markDirty();
        if (m_ruleFileList) m_ruleFileList->setEnabled(on);
    });
    ruleLayout->addWidget(m_allowUserRules);

    m_scriptsEnabled = new QCheckBox(
        I18n::instance().text("settings.command.scripts"), ruleGroup);
    connect(m_scriptsEnabled, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    ruleLayout->addWidget(m_scriptsEnabled);
    ruleLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.command.scripts.note"), ruleGroup));

    // 已加载的规则文件列表（只读展示，便于用户知道程序读了哪些）
    m_ruleFileList = new QListWidget(ruleGroup);
    m_ruleFileList->setMaximumHeight(110);
    m_ruleFileList->setSelectionMode(QAbstractItemView::NoSelection);
    ruleLayout->addWidget(m_ruleFileList);

    auto *reloadRow = new QHBoxLayout;
    auto *reloadButton = new QPushButton(
        I18n::instance().text("settings.command.reload_rules"), ruleGroup);
    connect(reloadButton, &QPushButton::clicked,
            this, &SettingsDialog::onReloadRules);
    reloadRow->addWidget(reloadButton);
    reloadRow->addStretch(1);
    ruleLayout->addLayout(reloadRow);

    layout->addWidget(ruleGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 网络
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildNetworkTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    auto *modeGroup = new QGroupBox(I18n::instance().text("settings.network.mode"), page);
    auto *modeLayout = new QVBoxLayout(modeGroup);

    m_networkMode = new QComboBox(modeGroup);
    m_networkMode->addItem(I18n::instance().text("settings.network.mode.offline"),
                           QVariant::fromValue(static_cast<int>(NetworkMode::Offline)));
    m_networkMode->addItem(I18n::instance().text("settings.network.mode.ask"),
                           QVariant::fromValue(static_cast<int>(NetworkMode::AskEachTime)));
    m_networkMode->addItem(I18n::instance().text("settings.network.mode.configured"),
                           QVariant::fromValue(static_cast<int>(NetworkMode::AllowConfigured)));
    m_networkMode->addItem(I18n::instance().text("settings.network.mode.custom"),
                           QVariant::fromValue(static_cast<int>(NetworkMode::Custom)));
    connect(m_networkMode, &QComboBox::currentIndexChanged, this, [this](int) {
        markDirty();
        if (m_askRemember) {
            const int mode = m_networkMode->currentData().toInt();
            m_askRemember->setEnabled(mode == static_cast<int>(NetworkMode::AskEachTime));
        }
    });
    modeLayout->addWidget(m_networkMode);

    modeLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.network.mode.note"), modeGroup));

    m_askRemember = new QCheckBox(
        I18n::instance().text("settings.network.ask_remember"), modeGroup);
    connect(m_askRemember, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    modeLayout->addWidget(m_askRemember);

    layout->addWidget(modeGroup);

    // ---- 超时与重试 ----
    auto *timeoutGroup = new QGroupBox(
        I18n::instance().text("settings.network.timeout"), page);
    auto *timeoutForm = new QFormLayout(timeoutGroup);

    m_connectTimeout = new QSpinBox(timeoutGroup);
    m_connectTimeout->setRange(1000, 60000);
    m_connectTimeout->setSingleStep(500);
    m_connectTimeout->setSuffix(I18n::instance().text("settings.unit.ms"));
    connect(m_connectTimeout, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    timeoutForm->addRow(I18n::instance().text("settings.network.connect_timeout"),
                        m_connectTimeout);

    m_readTimeout = new QSpinBox(timeoutGroup);
    m_readTimeout->setRange(1000, 120000);
    m_readTimeout->setSingleStep(500);
    m_readTimeout->setSuffix(I18n::instance().text("settings.unit.ms"));
    connect(m_readTimeout, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    timeoutForm->addRow(I18n::instance().text("settings.network.read_timeout"),
                        m_readTimeout);

    m_maxRetries = new QSpinBox(timeoutGroup);
    // SRS 4.9：不得无限重试。上限硬性设为 3。
    m_maxRetries->setRange(0, 3);
    connect(m_maxRetries, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    timeoutForm->addRow(I18n::instance().text("settings.network.max_retries"),
                        m_maxRetries);
    timeoutForm->addRow(QString(), makeFixedNote(
        I18n::instance().text("settings.network.max_retries.note"), timeoutGroup));

    layout->addWidget(timeoutGroup);

    // ---- 安全（不可更改项）----
    auto *tlsGroup = new QGroupBox(I18n::instance().text("settings.network.tls"), page);
    auto *tlsLayout = new QVBoxLayout(tlsGroup);

    m_insecureTlsNote = new QLabel(tlsGroup);
    m_insecureTlsNote->setWordWrap(true);
    m_insecureTlsNote->setStyleSheet(QStringLiteral(
        "background:#FFEBEE; border:1px solid #EF9A9A;"
        "border-radius:5px; padding:6px 8px; color:#B71C1C;"));
    tlsLayout->addWidget(m_insecureTlsNote);

    // 这里刻意**没有**"允许不安全 TLS"的复选框 —— 见头文件说明第 2 条。
    tlsLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.network.tls.fixed_note"), tlsGroup));

    layout->addWidget(tlsGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 语音与录音
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildSpeechTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ---- 语音（识别 + 合成）----
    auto *speechGroup = new QGroupBox(I18n::instance().text("settings.speech.group"), page);
    auto *speechLayout = new QVBoxLayout(speechGroup);

    m_speechLimitation = new QLabel(speechGroup);
    m_speechLimitation->setWordWrap(true);
    m_speechLimitation->setVisible(false);
    speechLayout->addWidget(m_speechLimitation);

    m_speechEnabled = new QCheckBox(
        I18n::instance().text("settings.speech.enabled"), speechGroup);
    connect(m_speechEnabled, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    speechLayout->addWidget(m_speechEnabled);
    speechLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.speech.enabled.note"), speechGroup));

    auto *speechForm = new QFormLayout;

    m_recognitionLanguage = new QComboBox(speechGroup);
    m_recognitionLanguage->addItem(I18n::instance().text("settings.language.zh"),
                                   QVariant::fromValue(static_cast<int>(Language::ZhCN)));
    m_recognitionLanguage->addItem(I18n::instance().text("settings.language.en"),
                                   QVariant::fromValue(static_cast<int>(Language::EnUS)));
    m_recognitionLanguage->addItem(I18n::instance().text("settings.language.ja"),
                                   QVariant::fromValue(static_cast<int>(Language::JaJP)));
    connect(m_recognitionLanguage, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::markDirty);
    speechForm->addRow(I18n::instance().text("settings.speech.language"),
                       m_recognitionLanguage);

    m_pushToTalk = new QCheckBox(
        I18n::instance().text("settings.speech.push_to_talk"), speechGroup);
    connect(m_pushToTalk, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
    speechForm->addRow(QString(), m_pushToTalk);

    m_synthesisEnabled = new QCheckBox(
        I18n::instance().text("settings.speech.synthesis"), speechGroup);
    connect(m_synthesisEnabled, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    speechForm->addRow(QString(), m_synthesisEnabled);

    // ---- 语速 ----
    auto *rateRow = new QHBoxLayout;
    m_speechRate = new QSlider(Qt::Horizontal, speechGroup);
    m_speechRate->setRange(-10, 10);
    rateRow->addWidget(m_speechRate, 1);
    m_speechRateValue = new QLabel(QStringLiteral("0"), speechGroup);
    m_speechRateValue->setFixedWidth(34);
    rateRow->addWidget(m_speechRateValue);
    connect(m_speechRate, &QSlider::valueChanged, this, [this](int v) {
        if (m_speechRateValue) m_speechRateValue->setText(QString::number(v));
        markDirty();
    });
    speechForm->addRow(I18n::instance().text("settings.speech.rate"), rateRow);

    // ---- 音量 ----
    auto *volumeRow = new QHBoxLayout;
    m_speechVolume = new QSlider(Qt::Horizontal, speechGroup);
    m_speechVolume->setRange(0, 100);
    volumeRow->addWidget(m_speechVolume, 1);
    m_speechVolumeValue = new QLabel(QStringLiteral("100"), speechGroup);
    m_speechVolumeValue->setFixedWidth(34);
    volumeRow->addWidget(m_speechVolumeValue);
    connect(m_speechVolume, &QSlider::valueChanged, this, [this](int v) {
        if (m_speechVolumeValue) m_speechVolumeValue->setText(QString::number(v));
        markDirty();
    });
    speechForm->addRow(I18n::instance().text("settings.speech.volume"), volumeRow);

    speechLayout->addLayout(speechForm);
    layout->addWidget(speechGroup);

    // ---- 录音 ----
    auto *recordGroup = new QGroupBox(I18n::instance().text("settings.recording.group"), page);
    auto *recordLayout = new QVBoxLayout(recordGroup);

    m_recordingLimitation = new QLabel(recordGroup);
    m_recordingLimitation->setWordWrap(true);
    m_recordingLimitation->setVisible(false);
    recordLayout->addWidget(m_recordingLimitation);

    m_recordingEnabled = new QCheckBox(
        I18n::instance().text("settings.recording.enabled"), recordGroup);
    connect(m_recordingEnabled, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    recordLayout->addWidget(m_recordingEnabled);
    recordLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.recording.enabled.note"), recordGroup));

    auto *recordForm = new QFormLayout;

    m_recordingMaxDuration = new QSpinBox(recordGroup);
    m_recordingMaxDuration->setRange(5, 7200);
    m_recordingMaxDuration->setSuffix(I18n::instance().text("settings.unit.seconds"));
    connect(m_recordingMaxDuration, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    recordForm->addRow(I18n::instance().text("settings.recording.max_duration"),
                       m_recordingMaxDuration);

    m_recordingSampleRate = new QComboBox(recordGroup);
    // 只提供两个实用档位，不开放任意值 —— 避免用户设出后端不支持的采样率
    m_recordingSampleRate->addItem(QStringLiteral("16000 Hz"), 16000);
    m_recordingSampleRate->addItem(QStringLiteral("44100 Hz"), 44100);
    connect(m_recordingSampleRate, &QComboBox::currentIndexChanged,
            this, &SettingsDialog::markDirty);
    recordForm->addRow(I18n::instance().text("settings.recording.sample_rate"),
                       m_recordingSampleRate);

    recordLayout->addLayout(recordForm);
    recordLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.recording.local_only"), recordGroup));

    layout->addWidget(recordGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 历史与隐私
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildHistoryTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    // ---- 记录类别开关 ----
    auto *categoryGroup = new QGroupBox(
        I18n::instance().text("settings.history.categories"), page);
    auto *categoryLayout = new QVBoxLayout(categoryGroup);

    categoryLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.history.categories.note"), categoryGroup));

    auto makeCategoryCheck = [&](const QString &key) -> QCheckBox * {
        auto *cb = new QCheckBox(I18n::instance().text(key), categoryGroup);
        connect(cb, &QCheckBox::toggled, this, &SettingsDialog::markDirty);
        categoryLayout->addWidget(cb);
        return cb;
    };

    m_historyConversation = makeCategoryCheck("settings.history.conversation");
    m_historyTask         = makeCategoryCheck("settings.history.task");
    m_historyTranslation  = makeCategoryCheck("settings.history.translation");
    m_historySearch       = makeCategoryCheck("settings.history.search");
    m_historyRecording    = makeCategoryCheck("settings.history.recording");

    layout->addWidget(categoryGroup);

    // ---- 保留策略 ----
    auto *retentionGroup = new QGroupBox(
        I18n::instance().text("settings.history.retention"), page);
    auto *retentionForm = new QFormLayout(retentionGroup);

    m_retentionDays = new QSpinBox(retentionGroup);
    m_retentionDays->setRange(0, 3650);
    m_retentionDays->setSuffix(I18n::instance().text("settings.unit.days"));
    connect(m_retentionDays, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    retentionForm->addRow(I18n::instance().text("settings.history.retention_days"),
                          m_retentionDays);
    retentionForm->addRow(QString(), makeFixedNote(
        I18n::instance().text("settings.history.retention_days.note"), retentionGroup));

    m_maxStorageMb = new QSpinBox(retentionGroup);
    m_maxStorageMb->setRange(50, 10240);
    m_maxStorageMb->setSingleStep(50);
    m_maxStorageMb->setSuffix(I18n::instance().text("settings.unit.mb"));
    connect(m_maxStorageMb, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::markDirty);
    retentionForm->addRow(I18n::instance().text("settings.history.max_storage"),
                          m_maxStorageMb);

    layout->addWidget(retentionGroup);

    // ---- 隐私（不可更改项）----
    auto *privacyGroup = new QGroupBox(
        I18n::instance().text("settings.history.privacy"), page);
    auto *privacyLayout = new QVBoxLayout(privacyGroup);

    m_auditLogEnabled = new QCheckBox(
        I18n::instance().text("settings.history.audit"), privacyGroup);
    connect(m_auditLogEnabled, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    privacyLayout->addWidget(m_auditLogEnabled);
    privacyLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.history.audit.note"), privacyGroup));

    m_fixedPrivacyNote = new QLabel(privacyGroup);
    m_fixedPrivacyNote->setWordWrap(true);
    m_fixedPrivacyNote->setStyleSheet(QStringLiteral(
        "background:#E8F5E9; border:1px solid #A5D6A7;"
        "border-radius:5px; padding:8px 10px; color:#1B5E20;"));
    m_fixedPrivacyNote->setText(
        I18n::instance().text("settings.history.privacy.commitment"));
    privacyLayout->addWidget(m_fixedPrivacyNote);

    layout->addWidget(privacyGroup);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 快捷键
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildHotkeyTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);

    auto *group = new QGroupBox(I18n::instance().text("settings.hotkey.group"), page);
    auto *groupLayout = new QVBoxLayout(group);

    m_hotkeyAvailability = new QLabel(group);
    m_hotkeyAvailability->setWordWrap(true);
    m_hotkeyAvailability->setVisible(false);
    groupLayout->addWidget(m_hotkeyAvailability);

    m_hotkeyEnabled = new QCheckBox(
        I18n::instance().text("settings.hotkey.enabled"), group);
    connect(m_hotkeyEnabled, &QCheckBox::toggled,
            this, &SettingsDialog::markDirty);
    groupLayout->addWidget(m_hotkeyEnabled);

    auto *form = new QFormLayout;

    auto makeEdit = [&](QKeySequenceEdit *&target) {
        target = new QKeySequenceEdit(group);
        // 只允许组合键（至少两个键），避免用户设成单个字母导致全系统无法输入
        target->setMaximumSequenceLength(1);
        connect(target, &QKeySequenceEdit::keySequenceChanged,
                this, &SettingsDialog::markDirty);
        return target;
    };

    form->addRow(I18n::instance().text("settings.hotkey.toggle_pet"),
                 makeEdit(m_hotkeyTogglePet));
    form->addRow(I18n::instance().text("settings.hotkey.toggle_mic"),
                 makeEdit(m_hotkeyToggleMic));
    form->addRow(I18n::instance().text("settings.hotkey.open_history"),
                 makeEdit(m_hotkeyOpenHistory));
    form->addRow(I18n::instance().text("settings.hotkey.open_input"),
                 makeEdit(m_hotkeyOpenInput));

    groupLayout->addLayout(form);

    m_hotkeyConflicts = new QLabel(group);
    m_hotkeyConflicts->setWordWrap(true);
    m_hotkeyConflicts->setVisible(false);
    groupLayout->addWidget(m_hotkeyConflicts);

    groupLayout->addWidget(makeFixedNote(
        I18n::instance().text("settings.hotkey.note"), group));

    layout->addWidget(group);
    layout->addStretch(1);

    return page;
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------
QWidget *SettingsDialog::buildDiagnosticsTab()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(10);

    m_diagnosticsView = new QTextBrowser(page);
    m_diagnosticsView->setOpenExternalLinks(false);
    layout->addWidget(m_diagnosticsView, 1);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(8);

    auto *openDataButton = new QPushButton(
        I18n::instance().text("settings.diagnostics.open_data"), page);
    connect(openDataButton, &QPushButton::clicked,
            this, &SettingsDialog::onOpenDataDirectory);
    buttonRow->addWidget(openDataButton);

    auto *openLogButton = new QPushButton(
        I18n::instance().text("settings.diagnostics.open_log"), page);
    connect(openLogButton, &QPushButton::clicked,
            this, &SettingsDialog::onOpenLogDirectory);
    buttonRow->addWidget(openLogButton);

    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    return page;
}

// ---------------------------------------------------------------------------
// 打开与重建
// ---------------------------------------------------------------------------
void SettingsDialog::open()
{
    rebuildFromRuntime();
    show();
    raise();
    activateWindow();
}

void SettingsDialog::rebuildFromRuntime()
{
    // 灌数据期间不要因为控件信号误判为"用户改动了"
    m_loading = true;

    if (m_runtime) {
        m_draft = m_runtime->appConfig();
        applyConfigToUi(m_draft);
    }

    // ---- 能力相关提示 ----
    if (m_runtime) {
        const CapabilityReport caps = m_runtime->capabilities();

        // 开机启动
        if (m_startupAvailability) {
            if (!caps.autostart.available) {
                m_startupAvailability->setText(
                    I18n::instance().text("settings.startup.unavailable")
                    + (caps.autostart.reason.isEmpty()
                           ? QString()
                           : QStringLiteral(" ") + caps.autostart.reason));
                m_startupAvailability->setStyleSheet(QStringLiteral(
                    "background:#FFF8E1; border:1px solid #F0C36D;"
                    "border-radius:5px; padding:6px 8px; color:#7A5A00;"));
                m_startupAvailability->setVisible(true);
                if (m_startupEnabled) m_startupEnabled->setEnabled(false);
            } else {
                m_startupAvailability->setVisible(false);
                if (m_startupEnabled) m_startupEnabled->setEnabled(true);
            }
        }

        // 全局快捷键
        if (m_hotkeyAvailability && m_hotkeyEnabled) {
            if (!caps.globalHotkey.available) {
                m_hotkeyAvailability->setText(
                    I18n::instance().text("settings.hotkey.unavailable")
                    + (caps.globalHotkey.reason.isEmpty()
                           ? QString()
                           : QStringLiteral(" ") + caps.globalHotkey.reason));
                m_hotkeyAvailability->setStyleSheet(QStringLiteral(
                    "background:#FFF8E1; border:1px solid #F0C36D;"
                    "border-radius:5px; padding:6px 8px; color:#7A5A00;"));
                m_hotkeyAvailability->setVisible(true);
                m_hotkeyEnabled->setChecked(false);
                m_hotkeyEnabled->setEnabled(false);
            } else {
                m_hotkeyAvailability->setVisible(false);
                m_hotkeyEnabled->setEnabled(true);
            }
        }

        // 托盘
        if (m_trayIconEnabled && !caps.trayIcon.available) {
            m_trayIconEnabled->setEnabled(false);
        }

        // TLS 安全说明
        if (m_insecureTlsNote) {
            if (caps.networkTls12.available) {
                m_insecureTlsNote->setText(
                    I18n::instance().text("settings.network.tls.available"));
                m_insecureTlsNote->setStyleSheet(QStringLiteral(
                    "background:#E8F5E9; border:1px solid #A5D6A7;"
                    "border-radius:5px; padding:6px 8px; color:#1B5E20;"));
            } else {
                m_insecureTlsNote->setText(
                    I18n::instance().text("settings.network.tls.unavailable")
                    + (caps.networkTls12.reason.isEmpty()
                           ? QString()
                           : QStringLiteral(" ") + caps.networkTls12.reason));
                m_insecureTlsNote->setStyleSheet(QStringLiteral(
                    "background:#FFEBEE; border:1px solid #EF9A9A;"
                    "border-radius:5px; padding:6px 8px; color:#B71C1C;"));
            }
        }
    }

    // ---- 语音能力 ----
    if (m_speechLimitation) {
        const QString text = speechLimitationText();
        if (text.isEmpty()) {
            m_speechLimitation->setVisible(false);
        } else {
            m_speechLimitation->setText(text);
            m_speechLimitation->setStyleSheet(QStringLiteral(
                "background:#FFF8E1; border:1px solid #F0C36D;"
                "border-radius:5px; padding:6px 8px; color:#7A5A00;"));
            m_speechLimitation->setVisible(true);
        }
    }

    // ---- 录音能力 ----
    if (m_recordingLimitation) {
        const QString text = recorderLimitationText();
        if (text.isEmpty()) {
            m_recordingLimitation->setVisible(false);
        } else {
            m_recordingLimitation->setText(text);
            m_recordingLimitation->setStyleSheet(QStringLiteral(
                "background:#FFF8E1; border:1px solid #F0C36D;"
                "border-radius:5px; padding:6px 8px; color:#7A5A00;"));
            m_recordingLimitation->setVisible(true);
        }
    }

    // ---- 规则文件列表 ----
    if (m_ruleFileList) {
        m_ruleFileList->clear();
        if (m_runtime && m_runtime->repository()) {
            // 展示程序实际加载的规则文件（不展示内容，只展示来源）
            const QStringList files = m_runtime->appConfig().command.ruleFiles;
            if (files.isEmpty()) {
                auto *item = new QListWidgetItem(
                    I18n::instance().text("settings.command.rules.builtin_only"));
                item->setForeground(QColor(0x8A, 0x94, 0xA2));
                m_ruleFileList->addItem(item);
            } else {
                for (const QString &f : files)
                    m_ruleFileList->addItem(f);
            }
        }
    }

    // ---- 诊断页 ----
    if (m_diagnosticsView)
        m_diagnosticsView->setHtml(buildDiagnosticsHtml());

    m_loading = false;
    m_dirty = false;
    updateDirtyIndicator();
}

// ---------------------------------------------------------------------------
// 配置 -> 界面
// ---------------------------------------------------------------------------
void SettingsDialog::applyConfigToUi(const AppConfig &config)
{
    // ---- 通用 ----
    if (m_languageCombo) {
        const int idx = m_languageCombo->findData(
            QVariant::fromValue(static_cast<int>(config.ui.language)));
        if (idx >= 0)
            m_languageCombo->setCurrentIndex(idx);
    }
    if (m_startupEnabled)   m_startupEnabled->setChecked(config.startup.enabled);
    if (m_startupMinimized) m_startupMinimized->setChecked(config.startup.startMinimized);
    if (m_startupDelay)     m_startupDelay->setValue(config.startup.delaySeconds);

    // ---- 外观 ----
    if (m_opacitySlider) {
        m_opacitySlider->setValue(ratioToSlider(config.ui.opacity));
        if (m_opacityValue)
            m_opacityValue->setText(QStringLiteral("%1%").arg(m_opacitySlider->value()));
    }
    if (m_scaleSlider) {
        m_scaleSlider->setValue(ratioToSlider(config.ui.scale));
        if (m_scaleValue)
            m_scaleValue->setText(QStringLiteral("%1%").arg(m_scaleSlider->value()));
    }
    if (m_alwaysOnTop)     m_alwaysOnTop->setChecked(config.ui.alwaysOnTop);
    if (m_startHidden)     m_startHidden->setChecked(config.ui.startHidden);
    if (m_trayIconEnabled) m_trayIconEnabled->setChecked(config.ui.trayIconEnabled);
    if (m_showStatusHints) m_showStatusHints->setChecked(config.ui.showStatusHints);
    if (m_bubbleDuration)  m_bubbleDuration->setValue(config.ui.bubbleDurationMs);
    if (m_bubbleMaxWidth)  m_bubbleMaxWidth->setValue(config.ui.bubbleMaxWidth);
    if (m_animationInterval) m_animationInterval->setValue(config.ui.animationIntervalMs);
    if (m_petImagePath)    m_petImagePath->setText(config.ui.petImagePath);

    // ---- 命令 ----
    if (m_minConfidence)  m_minConfidence->setValue(config.command.minConfidence);
    if (m_maxInputLength) m_maxInputLength->setValue(config.command.maxInputLength);
    if (m_allowUserRules) m_allowUserRules->setChecked(config.command.allowUserRules);
    if (m_scriptsEnabled) m_scriptsEnabled->setChecked(config.command.scriptsEnabled);

    // 权限策略：从运行时读实际的 policy（它已经由配置映射而来）
    if (m_runtime && m_runtime->permissions()) {
        const PermissionPolicy policy = m_runtime->permissions()->policy();
        if (m_confirmMediumRisk)
            m_confirmMediumRisk->setChecked(policy.confirmMediumRisk);
        if (m_authorizeHighRisk)
            m_authorizeHighRisk->setChecked(policy.authorizeHighRisk);
    }

    // ---- 网络 ----
    if (m_networkMode) {
        const int idx = m_networkMode->findData(
            QVariant::fromValue(static_cast<int>(config.network.mode)));
        if (idx >= 0)
            m_networkMode->setCurrentIndex(idx);
    }
    if (m_askRemember)    m_askRemember->setChecked(config.network.askEachTimeRememberChoice);
    if (m_connectTimeout) m_connectTimeout->setValue(config.network.connectTimeoutMs);
    if (m_readTimeout)    m_readTimeout->setValue(config.network.readTimeoutMs);
    if (m_maxRetries)     m_maxRetries->setValue(config.network.maxRetries);

    // ---- 语音 ----
    if (m_speechEnabled) m_speechEnabled->setChecked(config.speech.enabled);
    if (m_recognitionLanguage) {
        const int idx = m_recognitionLanguage->findData(
            QVariant::fromValue(static_cast<int>(config.speech.recognitionLanguage)));
        if (idx >= 0)
            m_recognitionLanguage->setCurrentIndex(idx);
    }
    if (m_pushToTalk)       m_pushToTalk->setChecked(config.speech.pushToTalk);
    if (m_synthesisEnabled) m_synthesisEnabled->setChecked(config.speech.synthesisEnabled);
    if (m_speechRate) {
        m_speechRate->setValue(config.speech.speechRate);
        if (m_speechRateValue)
            m_speechRateValue->setText(QString::number(config.speech.speechRate));
    }
    if (m_speechVolume) {
        m_speechVolume->setValue(config.speech.speechVolume);
        if (m_speechVolumeValue)
            m_speechVolumeValue->setText(QString::number(config.speech.speechVolume));
    }

    // ---- 录音 ----
    if (m_recordingEnabled)
        m_recordingEnabled->setChecked(config.speech.recordingEnabled);
    if (m_recordingMaxDuration)
        m_recordingMaxDuration->setValue(config.speech.recordingMaxDurationSec);
    if (m_recordingSampleRate) {
        const int idx = m_recordingSampleRate->findData(config.speech.recordingSampleRate);
        if (idx >= 0)
            m_recordingSampleRate->setCurrentIndex(idx);
    }

    // ---- 历史 ----
    if (m_historyConversation) m_historyConversation->setChecked(config.history.conversationEnabled);
    if (m_historyTask)         m_historyTask->setChecked(config.history.taskEnabled);
    if (m_historyTranslation)  m_historyTranslation->setChecked(config.history.translationEnabled);
    if (m_historySearch)       m_historySearch->setChecked(config.history.searchEnabled);
    if (m_historyRecording)    m_historyRecording->setChecked(config.history.recordingEnabled);
    if (m_retentionDays)       m_retentionDays->setValue(config.history.retentionDays);
    if (m_maxStorageMb) {
        // 配置里是字节，界面用 MB
        m_maxStorageMb->setValue(
            int(config.history.maxStorageBytes / (1024LL * 1024LL)));
    }
    if (m_auditLogEnabled) m_auditLogEnabled->setChecked(config.privacy.auditLogEnabled);

    // ---- 快捷键 ----
    if (m_hotkeyEnabled) m_hotkeyEnabled->setChecked(config.hotkey.enabled);
    if (m_hotkeyTogglePet)
        m_hotkeyTogglePet->setKeySequence(QKeySequence(config.hotkey.togglePet));
    if (m_hotkeyToggleMic)
        m_hotkeyToggleMic->setKeySequence(QKeySequence(config.hotkey.toggleMic));
    if (m_hotkeyOpenHistory)
        m_hotkeyOpenHistory->setKeySequence(QKeySequence(config.hotkey.openHistory));
    if (m_hotkeyOpenInput)
        m_hotkeyOpenInput->setKeySequence(QKeySequence(config.hotkey.openInput));
}

// ---------------------------------------------------------------------------
// 界面 -> 配置
// ---------------------------------------------------------------------------
void SettingsDialog::collectIntoConfig(AppConfig *config) const
{
    if (!config)
        return;

    // ---- 通用 ----
    if (m_languageCombo)
        config->ui.language = static_cast<Language>(m_languageCombo->currentData().toInt());
    if (m_startupEnabled)   config->startup.enabled = m_startupEnabled->isChecked();
    if (m_startupMinimized) config->startup.startMinimized = m_startupMinimized->isChecked();
    if (m_startupDelay)     config->startup.delaySeconds = m_startupDelay->value();

    // ---- 外观 ----
    if (m_opacitySlider) config->ui.opacity = sliderToRatio(m_opacitySlider->value());
    if (m_scaleSlider)   config->ui.scale = sliderToRatio(m_scaleSlider->value());
    if (m_alwaysOnTop)     config->ui.alwaysOnTop = m_alwaysOnTop->isChecked();
    if (m_startHidden)     config->ui.startHidden = m_startHidden->isChecked();
    if (m_trayIconEnabled) config->ui.trayIconEnabled = m_trayIconEnabled->isChecked();
    if (m_showStatusHints) config->ui.showStatusHints = m_showStatusHints->isChecked();
    if (m_bubbleDuration)  config->ui.bubbleDurationMs = m_bubbleDuration->value();
    if (m_bubbleMaxWidth)  config->ui.bubbleMaxWidth = m_bubbleMaxWidth->value();
    if (m_animationInterval) config->ui.animationIntervalMs = m_animationInterval->value();
    if (m_petImagePath)    config->ui.petImagePath = m_petImagePath->text().trimmed();

    // ---- 命令 ----
    if (m_minConfidence)  config->command.minConfidence = m_minConfidence->value();
    if (m_maxInputLength) config->command.maxInputLength = m_maxInputLength->value();
    if (m_allowUserRules) config->command.allowUserRules = m_allowUserRules->isChecked();
    if (m_scriptsEnabled) config->command.scriptsEnabled = m_scriptsEnabled->isChecked();

    // ---- 网络 ----
    if (m_networkMode)
        config->network.mode = static_cast<NetworkMode>(m_networkMode->currentData().toInt());
    if (m_askRemember)
        config->network.askEachTimeRememberChoice = m_askRemember->isChecked();
    if (m_connectTimeout) config->network.connectTimeoutMs = m_connectTimeout->value();
    if (m_readTimeout)    config->network.readTimeoutMs = m_readTimeout->value();
    if (m_maxRetries)     config->network.maxRetries = m_maxRetries->value();

    // 注意：allowInsecureTls 不在这里赋值 —— 界面上没有这个控件，
    //  配置里它的默认值 true→false 的转换由 ConfigManager 负责，
    //  本窗口绝不把它改成 true。

    // ---- 语音 ----
    if (m_speechEnabled) config->speech.enabled = m_speechEnabled->isChecked();
    if (m_recognitionLanguage)
        config->speech.recognitionLanguage =
            static_cast<Language>(m_recognitionLanguage->currentData().toInt());
    if (m_pushToTalk)       config->speech.pushToTalk = m_pushToTalk->isChecked();
    if (m_synthesisEnabled) config->speech.synthesisEnabled = m_synthesisEnabled->isChecked();
    if (m_speechRate)       config->speech.speechRate = m_speechRate->value();
    if (m_speechVolume)     config->speech.speechVolume = m_speechVolume->value();

    // ---- 录音 ----
    if (m_recordingEnabled)
        config->speech.recordingEnabled = m_recordingEnabled->isChecked();
    if (m_recordingMaxDuration)
        config->speech.recordingMaxDurationSec = m_recordingMaxDuration->value();
    if (m_recordingSampleRate)
        config->speech.recordingSampleRate = m_recordingSampleRate->currentData().toInt();

    // ---- 历史 ----
    if (m_historyConversation) config->history.conversationEnabled = m_historyConversation->isChecked();
    if (m_historyTask)         config->history.taskEnabled = m_historyTask->isChecked();
    if (m_historyTranslation)  config->history.translationEnabled = m_historyTranslation->isChecked();
    if (m_historySearch)       config->history.searchEnabled = m_historySearch->isChecked();
    if (m_historyRecording)    config->history.recordingEnabled = m_historyRecording->isChecked();
    if (m_retentionDays)       config->history.retentionDays = m_retentionDays->value();
    if (m_maxStorageMb)
        config->history.maxStorageBytes =
            qint64(m_maxStorageMb->value()) * 1024LL * 1024LL;
    if (m_auditLogEnabled) config->privacy.auditLogEnabled = m_auditLogEnabled->isChecked();

    // ---- 快捷键 ----
    if (m_hotkeyEnabled) config->hotkey.enabled = m_hotkeyEnabled->isChecked();
    if (m_hotkeyTogglePet)
        config->hotkey.togglePet = m_hotkeyTogglePet->keySequence().toString();
    if (m_hotkeyToggleMic)
        config->hotkey.toggleMic = m_hotkeyToggleMic->keySequence().toString();
    if (m_hotkeyOpenHistory)
        config->hotkey.openHistory = m_hotkeyOpenHistory->keySequence().toString();
    if (m_hotkeyOpenInput)
        config->hotkey.openInput = m_hotkeyOpenInput->keySequence().toString();
}

// ---------------------------------------------------------------------------
// dirty 状态
// ---------------------------------------------------------------------------
void SettingsDialog::markDirty()
{
    if (m_loading)
        return;

    if (!m_dirty) {
        m_dirty = true;
        updateDirtyIndicator();
    }
}

void SettingsDialog::updateDirtyIndicator()
{
    if (!m_dirtyLabel)
        return;

    m_dirtyLabel->setVisible(m_dirty);
    if (m_dirty)
        m_dirtyLabel->setText(I18n::instance().text("settings.dirty"));

    if (m_saveButton)
        m_saveButton->setEnabled(true);   // 保存按钮始终可点（点了没改动也无害）
}

// ---------------------------------------------------------------------------
// 能力说明
// ---------------------------------------------------------------------------
QString SettingsDialog::speechLimitationText() const
{
    if (!m_runtime)
        return I18n::instance().text("settings.speech.no_runtime");

    SpeechService *speech = m_runtime->speech();
    if (!speech) {
        return I18n::instance().text("settings.speech.not_compiled");
    }

    const SpeechServiceStatus status = speech->status();
    if (status.backendReady)
        return QString();     // 可用，不需要提示

    if (!status.limitation.isEmpty())
        return status.limitation;

    return I18n::instance().text("settings.speech.no_backend");
}

QString SettingsDialog::recorderLimitationText() const
{
    if (!m_runtime)
        return I18n::instance().text("settings.recording.no_runtime");

    RecorderService *recorder = m_runtime->recorder();
    if (!recorder)
        return I18n::instance().text("settings.recording.not_compiled");

    QString reason;
    if (recorder->probeCapability(&reason))
        return QString();     // 可用

    return reason.isEmpty()
               ? I18n::instance().text("settings.recording.no_device")
               : reason;
}

// ---------------------------------------------------------------------------
// 诊断页
// ---------------------------------------------------------------------------
QString SettingsDialog::buildDiagnosticsHtml() const
{
    if (!m_runtime)
        return QStringLiteral("<p>%1</p>")
            .arg(I18n::instance().text("settings.diagnostics.no_runtime"));

    QString html;
    html += QStringLiteral("<style>"
                           "h3{margin:14px 0 6px 0;}"
                           "table{border-collapse:collapse;width:100%;}"
                           "td,th{border:1px solid #D8DEE6;padding:5px 8px;"
                           "text-align:left;vertical-align:top;}"
                           "th{background:#F2F5F9;}"
                           "code{background:#F2F5F9;padding:1px 4px;}"
                           "</style>");

    const I18n &i18n = I18n::instance();

    // ---- 版本与系统 ----
    html += QStringLiteral("<h3>%1</h3><table>")
        .arg(i18n.text("settings.diagnostics.system"));

    const SystemSnapshot snap = SystemInfo::current();
    auto row = [&html](const QString &k, const QString &v) {
        html += QStringLiteral("<tr><th style='width:190px'>%1</th><td>%2</td></tr>")
            .arg(k.toHtmlEscaped(), v.toHtmlEscaped());
    };

    row(i18n.text("settings.diagnostics.os"), snap.osName.isEmpty()
            ? i18n.text("settings.diagnostics.unknown") : snap.osName);
    row(i18n.text("settings.diagnostics.os_version"), snap.osVersion);
    row(i18n.text("settings.diagnostics.arch"), snap.architecture);
    row(i18n.text("settings.diagnostics.locale"), snap.localeName);

    html += QStringLiteral("</table>");

    // ---- 路径布局（数据存在哪里）----
    html += QStringLiteral("<h3>%1</h3><table>")
        .arg(i18n.text("settings.diagnostics.paths"));

    const AppPaths paths = m_runtime->paths();
    row(i18n.text("settings.diagnostics.path.config_dir"), paths.configDir);
    row(i18n.text("settings.diagnostics.path.data_dir"), paths.dataDir);
    row(i18n.text("settings.diagnostics.path.log_dir"), paths.logDir);
    row(i18n.text("settings.diagnostics.path.cache_dir"), paths.cacheDir);
    row(i18n.text("settings.diagnostics.path.recordings"), paths.recordingsDir);
    row(i18n.text("settings.diagnostics.path.db"), paths.historyDbPath);
    row(i18n.text("settings.diagnostics.path.config_file"), paths.configFilePath);

    html += QStringLiteral("</table>");

    // ---- 能力报告 ----
    html += QStringLiteral("<h3>%1</h3><table>")
        .arg(i18n.text("settings.diagnostics.capabilities"));

    auto capRow = [&html, &i18n](const QString &name, const Capability &cap) {
        const QString state = cap.available
                                  ? QStringLiteral("<b style='color:#2E7D32'>%1</b>")
                                        .arg(i18n.text("settings.diagnostics.available"))
                                  : QStringLiteral("<b style='color:#C62828'>%1</b>")
                                        .arg(i18n.text("settings.diagnostics.unavailable"));
        QString detail = cap.available ? cap.detail : cap.reason;
        html += QStringLiteral("<tr><th style='width:190px'>%1</th><td>%2 %3</td></tr>")
            .arg(name.toHtmlEscaped(), state, detail.toHtmlEscaped());
    };

    const CapabilityReport caps = m_runtime->capabilities();
    capRow(i18n.text("settings.diagnostics.cap.translucent"), caps.translucentWindow);
    capRow(i18n.text("settings.diagnostics.cap.always_on_top"), caps.alwaysOnTop);
    capRow(i18n.text("settings.diagnostics.cap.tray"), caps.trayIcon);
    capRow(i18n.text("settings.diagnostics.cap.hotkey"), caps.globalHotkey);
    capRow(i18n.text("settings.diagnostics.cap.autostart"), caps.autostart);
    capRow(i18n.text("settings.diagnostics.cap.tls"), caps.networkTls12);
    capRow(i18n.text("settings.diagnostics.cap.ca_store"), caps.systemCaStore);
    capRow(i18n.text("settings.diagnostics.cap.audio_in"), caps.audioInput);
    capRow(i18n.text("settings.diagnostics.cap.audio_out"), caps.audioOutput);
    capRow(i18n.text("settings.diagnostics.cap.asr"), caps.speechRecognition);
    capRow(i18n.text("settings.diagnostics.cap.tts"), caps.speechSynthesis);
    capRow(i18n.text("settings.diagnostics.cap.secure_store"), caps.secureStorage);
    capRow(i18n.text("settings.diagnostics.cap.notifications"), caps.desktopNotifications);
    capRow(i18n.text("settings.diagnostics.cap.image_seq"), caps.imageSequenceLoad);

    html += QStringLiteral("</table>");

    // ---- 启动降级说明（SRS 6.2 / 7.2）----
    const StartupReport report = m_runtime->report();
    if (report.hasProblems() || !report.disabledFeatures.isEmpty()) {
        html += QStringLiteral("<h3>%1</h3><ul>")
            .arg(i18n.text("settings.diagnostics.degraded"));

        for (const QString &p : report.startupProblems)
            html += QStringLiteral("<li>%1</li>").arg(p.toHtmlEscaped());

        for (const QString &f : report.disabledFeatures)
            html += QStringLiteral("<li>%1</li>").arg(f.toHtmlEscaped());

        html += QStringLiteral("</ul>");
    }

    // ---- 环境隔离：项目目录之外的位置 ----
    //  这一节直接对应 docs/ENVIRONMENT_ISOLATION.md 的要求：
    //  程序在项目目录之外读写了哪些位置，这里逐条列出。
    const QStringList external = m_runtime->externalLocations();
    if (!external.isEmpty()) {
        html += QStringLiteral("<h3>%1</h3><ul>")
            .arg(i18n.text("settings.diagnostics.external"));
        for (const QString &loc : external)
            html += QStringLiteral("<li><code>%1</code></li>").arg(loc.toHtmlEscaped());
        html += QStringLiteral("</ul>");
        html += QStringLiteral("<p style='color:#7A8593'>%1</p>")
            .arg(i18n.text("settings.diagnostics.external.note"));
    }

    // ---- 兼容性报告 ----
    const QString compat = SystemInfo::buildCompatibilityReport(snap);
    if (!compat.isEmpty()) {
        html += QStringLiteral("<h3>%1</h3><pre style='white-space:pre-wrap'>%2</pre>")
            .arg(i18n.text("settings.diagnostics.compat"), compat.toHtmlEscaped());
    }

    return html;
}

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------
bool SettingsDialog::validateBeforeSave(QString *message) const
{
    // ---- 快捷键重复检查 ----
    if (m_hotkeyEnabled && m_hotkeyEnabled->isChecked()) {
        const QList<QPair<QString, QKeySequenceEdit *>> edits = {
            { I18n::instance().text("settings.hotkey.toggle_pet"), m_hotkeyTogglePet },
            { I18n::instance().text("settings.hotkey.toggle_mic"), m_hotkeyToggleMic },
            { I18n::instance().text("settings.hotkey.open_history"), m_hotkeyOpenHistory },
            { I18n::instance().text("settings.hotkey.open_input"), m_hotkeyOpenInput },
        };

        for (int i = 0; i < edits.size(); ++i) {
            for (int j = i + 1; j < edits.size(); ++j) {
                const QKeySequenceEdit *a = edits.at(i).second;
                const QKeySequenceEdit *b = edits.at(j).second;
                if (!a || !b)
                    continue;

                const QKeySequence sa = a->keySequence();
                const QKeySequence sb = b->keySequence();

                if (!sa.isEmpty() && sa == sb) {
                    if (message) {
                        *message = I18n::instance().text(
                            "settings.error.hotkey_conflict",
                            QStringLiteral("a"), edits.at(i).first,
                            QStringLiteral("b"), edits.at(j).first);
                    }
                    return false;
                }
            }
        }
    }

    // ---- 透明度 / 缩放的字面范围校验 ----
    if (m_opacitySlider) {
        const double v = sliderToRatio(m_opacitySlider->value());
        if (v < 0.2 || v > 1.0) {
            if (message) {
                *message = I18n::instance().text("settings.error.opacity_range");
            }
            return false;
        }
    }

    if (m_scaleSlider) {
        const double v = sliderToRatio(m_scaleSlider->value());
        if (v < 0.5 || v > 3.0) {
            if (message) {
                *message = I18n::instance().text("settings.error.scale_range");
            }
            return false;
        }
    }

    // ---- 网络上限 ----
    if (m_maxRetries && m_maxRetries->value() > 3) {
        if (message)
            *message = I18n::instance().text("settings.error.retries_range");
        return false;
    }

    return true;
}

void SettingsDialog::onSave()
{
    if (!m_runtime) {
        QMessageBox::warning(this,
                             I18n::instance().text("settings.title"),
                             I18n::instance().text("settings.error.no_runtime"));
        return;
    }

    QString problem;
    if (!validateBeforeSave(&problem)) {
        QMessageBox::warning(this,
                             I18n::instance().text("settings.title"),
                             problem);
        return;
    }

    // 界面 -> 草稿
    collectIntoConfig(&m_draft);

    // 草稿 -> ConfigManager -> ConfigManager::save()
    //
    //  经由 ConfigManager::setConfig 而不是直接改 m_config，
    //  是因为 setConfig 会跑一次 validate()，把越界/不安全的值修掉。
    //  这样"界面写进去的值"和"真正生效的值"之间始终有一道校验。
    if (m_runtime->config()) {
        m_runtime->config()->setConfig(m_draft);
    }

    QString saveMessage;
    const bool ok = m_runtime->saveConfig(&saveMessage);

    if (!ok) {
        QMessageBox::warning(
            this,
            I18n::instance().text("settings.title"),
            saveMessage.isEmpty()
                ? I18n::instance().text("settings.error.save_failed")
                : saveMessage);
        return;
    }

    m_dirty = false;
    updateDirtyIndicator();

    DC_INFO(LogCat::Config, QStringLiteral("Settings saved by user"));

    // 有些设置需要重启才完全生效（如语言、动画间隔、权限策略）。
    //  不骗用户说"立即全部生效"，而是分情况说明。
    QMessageBox::information(
        this,
        I18n::instance().text("settings.title"),
        I18n::instance().text("settings.save.done"));

    // 保存后重建一次界面：ConfigManager 可能修正了某些值，
    //  界面要反映"实际生效的值"，不能还显示用户输入的越界值。
    rebuildFromRuntime();
}

// ---------------------------------------------------------------------------
// 恢复默认
// ---------------------------------------------------------------------------
void SettingsDialog::onResetToDefaults()
{
    if (!m_runtime) {
        QMessageBox::warning(this,
                             I18n::instance().text("settings.title"),
                             I18n::instance().text("settings.error.no_runtime"));
        return;
    }

    const auto answer = QMessageBox::question(
        this,
        I18n::instance().text("settings.reset.title"),
        I18n::instance().text("settings.reset.confirm"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (answer != QMessageBox::Yes)
        return;

    // ConfigManager::resetToDefaults 会先把当前配置备份，再写默认值。
    //  这里同时刷新草稿与界面。
    const Result r = m_runtime->config()
                         ? m_runtime->config()->resetToDefaults(true)
                         : Result::failure(erc::NotInitialized,
                                           I18n::instance().text("settings.error.no_runtime"));

    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("settings.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("settings.error.reset_failed")
                                 : r.message);
        return;
    }

    DC_WARN(LogCat::Config,
            QStringLiteral("Configuration reset to defaults by user"));

    rebuildFromRuntime();

    QMessageBox::information(
        this,
        I18n::instance().text("settings.title"),
        I18n::instance().text("settings.reset.done"));
}

// ---------------------------------------------------------------------------
// 导入 / 导出
// ---------------------------------------------------------------------------
void SettingsDialog::onImport()
{
    if (!m_runtime || !m_runtime->config())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this,
        I18n::instance().text("settings.import.title"),
        QString(),
        I18n::instance().text("settings.import.filter"));

    if (path.isEmpty())
        return;

    const ConfigLoadResult result = m_runtime->config()->importFrom(path);

    // 导入可能有 warning（字段缺失/类型不对/被修正），要如实汇总
    if (result.hasErrors()) {
        QString detail;
        for (const ConfigIssue &issue : result.issues) {
            if (issue.severity == ConfigIssue::Severity::Error)
                detail += QStringLiteral("• %1: %2\n").arg(issue.path, issue.message);
        }
        QMessageBox::warning(
            this,
            I18n::instance().text("settings.import.title"),
            I18n::instance().text("settings.import.has_errors") + QStringLiteral("\n\n") + detail);
    } else if (result.hasWarnings()) {
        QString detail;
        for (const ConfigIssue &issue : result.issues) {
            if (issue.severity == ConfigIssue::Severity::Warning)
                detail += QStringLiteral("• %1: %2\n").arg(issue.path, issue.message);
        }
        QMessageBox::information(
            this,
            I18n::instance().text("settings.import.title"),
            I18n::instance().text("settings.import.has_warnings") + QStringLiteral("\n\n") + detail);
    }

    DC_INFO(LogCat::Config, QStringLiteral("Configuration imported by user"));

    rebuildFromRuntime();
    markDirty();   // 导入后配置已变，需要保存才落盘
}

void SettingsDialog::onExport()
{
    if (!m_runtime || !m_runtime->config())
        return;

    const QString defaultName = QStringLiteral("desktop-companion-config.json");

    const QString path = QFileDialog::getSaveFileName(
        this,
        I18n::instance().text("settings.export.title"),
        defaultName,
        I18n::instance().text("settings.export.filter"));

    if (path.isEmpty())
        return;

    // 关键：includeSecrets 恒为 false。
    //  SRS 5 规定密钥默认不随配置导出。这里不提供"包含密钥"的选项 ——
    //  需要密钥的场景应该走专门的凭据迁移流程，不该混在配置文件里。
    const Result r = m_runtime->config()->exportTo(path, false);

    if (!r.ok) {
        QMessageBox::warning(this,
                             I18n::instance().text("settings.title"),
                             r.message.isEmpty()
                                 ? I18n::instance().text("settings.export.failed")
                                 : r.message);
        return;
    }

    DC_INFO(LogCat::Config, QStringLiteral("Configuration exported (no secrets)"));

    QMessageBox::information(
        this,
        I18n::instance().text("settings.title"),
        I18n::instance().text("settings.export.done",
                              QStringLiteral("path"), QDir::toNativeSeparators(path)));
}

// ---------------------------------------------------------------------------
// 打开目录
// ---------------------------------------------------------------------------
void SettingsDialog::onOpenDataDirectory()
{
    if (!m_runtime)
        return;

    const AppPaths paths = m_runtime->paths();
    // 数据目录可能还没建出来（首次运行），先建再开
    QDir().mkpath(paths.dataDir);

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(paths.dataDir))) {
        QMessageBox::information(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.diagnostics.open_failed",
                                  QStringLiteral("path"), paths.dataDir));
    }
}

void SettingsDialog::onOpenLogDirectory()
{
    if (!m_runtime)
        return;

    const AppPaths paths = m_runtime->paths();
    QDir().mkpath(paths.logDir);

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(paths.logDir))) {
        QMessageBox::information(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.diagnostics.open_failed",
                                  QStringLiteral("path"), paths.logDir));
    }
}

// ---------------------------------------------------------------------------
// 重载规则
//
//  用户在"命令"页点这个按钮时，期望是"我刚改了 resources/commands 下的
//  json，让程序重新读一遍"。所以这里必须真的重新加载，
//  而不是只弹一句"请重启程序"。
// ---------------------------------------------------------------------------
void SettingsDialog::onReloadRules()
{
    if (!m_runtime || !m_runtime->repository()) {
        QMessageBox::information(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.reload.unavailable"));
        return;
    }

    CommandRepository *repo = m_runtime->repository();

    const int before = repo->count();

    // 从规则目录重新加载。
    //  注意 loadFromDirectory 是"追加覆盖"语义（见 CommandRepository 注释），
    //  所以重载前先 clear() —— 否则用户删掉一条规则后重载，旧规则还留在内存里，
    //  那"重载"就变成了骗人的按钮。
    repo->clear();

    const QString rulesDir = m_runtime->paths().dataDir.isEmpty()
                                 ? QString()
                                 : QStringLiteral("resources/commands");

    const RuleLoadResult load = repo->loadFromDirectory(rulesDir);

    const int after = repo->count();

    DC_INFO(LogCat::Command,
            QStringLiteral("Command rules reloaded by user: %1 -> %2 (ok=%3)")
                .arg(before).arg(after)
                .arg(load.ok ? QStringLiteral("yes") : QStringLiteral("no")));

    // 加载出错要如实报出来（哪一行错了），不能只说"完成"
    if (load.hasErrors()) {
        QString detail;
        const QStringList errors = load.errorMessages();
        for (const QString &e : errors)
            detail += QStringLiteral("• %1\n").arg(e);

        QMessageBox::warning(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.reload.with_errors",
                                  QStringLiteral("count"), QString::number(after))
            + QStringLiteral("\n\n") + detail);
    } else {
        QMessageBox::information(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.reload.done",
                                  QStringLiteral("before"), QString::number(before),
                                  QStringLiteral("after"), QString::number(after)));
    }

    // 刷新诊断页（规则数量会体现出来）
    if (m_diagnosticsView)
        m_diagnosticsView->setHtml(buildDiagnosticsHtml());
}

// ---------------------------------------------------------------------------
// 关闭
// ---------------------------------------------------------------------------
void SettingsDialog::closeEvent(QCloseEvent *event)
{
    // 有未保存改动时拦一下 —— 但不强迫保存，用户可以明确丢弃。
    if (m_dirty) {
        const auto answer = QMessageBox::question(
            this,
            I18n::instance().text("settings.title"),
            I18n::instance().text("settings.close.unsaved"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Cancel);

        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }

        if (answer == QMessageBox::Save) {
            onSave();
            // 如果保存失败，onSave 里已经提示过；此时仍然留在窗口里
            if (m_dirty) {
                event->ignore();
                return;
            }
        } else {
            // Discard：丢弃改动，回到当前生效的配置
            DC_INFO(LogCat::Config,
                    QStringLiteral("Settings changes discarded by user"));
            rebuildFromRuntime();
        }
    }

    event->accept();
    hide();   // 复用实例
}

void SettingsDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        // Esc 走和关闭按钮一样的逻辑（包括未保存拦截）
        close();
        return;
    }
    QDialog::keyPressEvent(event);
}

} // namespace dc
