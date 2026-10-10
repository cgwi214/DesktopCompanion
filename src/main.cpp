// ============================================================================
//  main.cpp — 程序入口与顶层装配
//
//  需求依据：SRS V1.1
//    3.2：UI 层不得直接执行系统命令或访问网络
//    3.3：核心模块划分
//    6.2：能力探测控制功能入口
//    7.2：单个模块失败不得导致核心退出（降级运行）
//    8.2：便携包 —— 命令行参数 --portable 启用
//
//  本文件是唯一同时看到"全部 UI 组件"和"CoreRuntime"的地方。
//  它的职责只有四件：
//
//    1. 解析命令行（--portable 等）
//    2. 启动 CoreRuntime，拿到启动报告
//    3. 创建 UI 组件，把它们的信号接到一起
//    4. 把 CoreRuntime 的信号接到 UI 上
//
//  它**不做**任何业务判断 —— 没有 if (intent == ...)，没有服务调用。
//  所有"点这个按钮会发生什么"都在 IntentRouter 里。
//
//  为什么 UI 组件的创建不放进 CoreRuntime：
//    CoreRuntime 是"无界面的核心"。把它和 QWidget 绑在一起，
//    单元测试就没法只测核心了（需要 QApplication + 显示器）。
//    保持这个边界，是让 SRS 3.2 那条规则可验证的前提。
//
//  ────────────────────────────────────────────────────────────────────────
//  信号连接总览（全部集中在这里，方便一眼看清界面与核心的关系）：
//
//    PetWindow.activated          -> 打开输入框
//    PetWindow.inputRequested     -> 打开输入框
//    PetWindow.historyRequested   -> 打开历史
//    PetWindow.settingsRequested  -> 打开设置
//    PetWindow.quitRequested      -> 退出
//    PetWindow.positionChanged    -> 保存位置到配置
//
//    CommandInputBox.submitted    -> IntentRouter.handle()
//
//    TrayController.*             -> 同上（托盘按钮走完全一样的路径）
//
//    IntentRouter.authorizationRequired -> 弹权限对话框
//    IntentRouter.interactionFinished   -> 气泡反馈 + 状态机
//
//    HotkeyManager.hotkeyTriggered -> 分发到各动作
//
//    PermissionDialog.decisionMade -> IntentRouter.resolveAuthorization()
//  ────────────────────────────────────────────────────────────────────────
// ============================================================================
#include "core/BuildConfig.h"
#include "core/CoreRuntime.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"
// main.cpp 要真正调用 router->handle() / resolveAuthorization()，还要访问
// InteractionRequest / InteractionOutcome 的字段。CoreRuntime.h 里对 IntentRouter
// 只有一句前向声明（那是给服务层解耦用的），所以这里必须包含真头文件——
// 只靠前向声明会报 "incomplete type 'dc::IntentRouter' used in nested name specifier"。
#include "engine/IntentRouter.h"
#include "model/Types.h"
#include "platform/AutostartManager.h"
#include "platform/HotkeyManager.h"
#include "platform/SystemInfo.h"
#include "services/SystemService.h"

#include "ui/CommandInputBox.h"
#include "ui/HistoryDialog.h"
#include "ui/I18n.h"
#include "ui/PermissionDialog.h"
#include "ui/PetRenderer.h"
#include "ui/PetStateMachine.h"
#include "ui/PetWindow.h"
#include "ui/RecorderPanel.h"
#include "ui/SettingsDialog.h"
#include "ui/SpeechBubble.h"
#include "ui/TrayController.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QMessageBox>
#include <QScreen>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>

using namespace dc;

namespace {

// ---------------------------------------------------------------------------
// 顶层控制器
//
//  把"信号接线"集中成一个小类，而不是在 main() 里写两百行 connect。
//  好处：
//    - main() 短到能一眼读完
//    - 所有 UI 对象的生命周期由成员变量统一管理（RAII）
//    - 需要改接线时只有一个地方要动
//
//  它不是"又一个业务层" —— 它没有任何业务逻辑，
//  每个槽函数都是一行转发。
// ---------------------------------------------------------------------------
class AppController : public QObject
{
    Q_OBJECT

public:
    AppController(CoreRuntime *runtime, QObject *parent = nullptr)
        : QObject(parent)
        , m_runtime(runtime)
    {
    }

    ~AppController() override = default;

    // 创建全部 UI 组件并接线。返回 false 表示连最小可用界面都建不起来。
    bool initialize()
    {
        if (!m_runtime || !m_runtime->isReady()) {
            DC_ERROR(LogCat::Core,
                     QStringLiteral("AppController::initialize called before runtime ready"));
            return false;
        }

        const AppConfig config = m_runtime->appConfig();
        const CapabilityReport caps = m_runtime->capabilities();

        createRendererAndWindow(config, caps);
        createInputBox(config);
        createHistoryAndSettings();
        createRecorder();
        createTray(config, caps);
        createHotkeys(config);

        connectRuntimeSignals();
        connectUiSignals();

        return true;
    }

    // 启动完成后的收尾：显示桌宠、启动调度器、处理开机启动同步
    void start()
    {
        if (m_petWindow) {
            if (m_runtime->appConfig().ui.startHidden) {
                // 配置要求"启动后隐藏"：不显示窗口，但托盘还在。
                //  用户可以从托盘把它叫回来。
                DC_INFO(LogCat::Ui,
                        QStringLiteral("Starting with pet window hidden per config"));
            } else {
                m_petWindow->showPet();
            }
        }

        if (m_tray)
            m_tray->show();

        // 同步开机启动状态（SRS 4.12：配置与实际状态可能不一致 ——
        //  用户可能手工删掉了启动项，或者换了程序位置）
        syncAutostart();

        // 首次安装/首次运行时，用气泡打个招呼并说明关键操作方式。
        //  这不是装饰：桌宠没有任何窗口边框和菜单，
        //  不告诉用户"双击可以输入命令"，用户很可能不知道怎么用。
        if (m_petWindow) {
            const QString welcome = I18n::instance().text("app.welcome");
            m_petWindow->say(welcome, 7000);
        }
    }

    // 遍历所有 UI 对象，确保退出时按正确顺序销毁
    void shutdown()
    {
        DC_INFO(LogCat::Core, QStringLiteral("AppController shutting down UI"));

        // 顺序说明：
        //   先注销全局快捷键 —— 否则退出过程中热键还活着，
        //   用户按一下可能触发一个正在析构的对象。
        if (m_hotkeys)
            m_hotkeys->unregisterAll();

        // 再停托盘：托盘图标的生命周期不能在 QApplication 之后结束
        if (m_tray) {
            m_tray->hide();
            delete m_tray;
            m_tray = nullptr;
        }

        // 用 deleteLater 语义太晚（事件循环已在退出），直接析构。
        //  这里用完就置空，避免二次删除。
        delete m_speechBubble;  m_speechBubble = nullptr;
        delete m_inputBox;      m_inputBox = nullptr;
        delete m_permissionDialog; m_permissionDialog = nullptr;
        delete m_historyDialog; m_historyDialog = nullptr;
        delete m_settingsDialog; m_settingsDialog = nullptr;
        delete m_recorderPanel; m_recorderPanel = nullptr;
        delete m_petWindow;     m_petWindow = nullptr;
        delete m_stateMachine;  m_stateMachine = nullptr;
        delete m_renderer;      m_renderer = nullptr;
        delete m_hotkeys;       m_hotkeys = nullptr;
        delete m_autostart;     m_autostart = nullptr;
    }

    PetWindow *petWindow() const { return m_petWindow; }
    CommandInputBox *inputBox() const { return m_inputBox; }
    TrayController *tray() const { return m_tray; }

private:
    // -----------------------------------------------------------------------
    // 创建：桌宠形象与主窗口
    // -----------------------------------------------------------------------
    void createRendererAndWindow(const AppConfig &config,
                                 const CapabilityReport &caps)
    {
        // ---- 渲染器 ----
        m_renderer = new PetRenderer(this);

        PetRenderConfig rendererConfig;
        rendererConfig.skinId = config.ui.petSkinId;
        rendererConfig.skinsRoot = QCoreApplication::applicationDirPath()
                                   + QStringLiteral("/resources/pets");
        rendererConfig.staticImagePath = config.ui.petImagePath;
        rendererConfig.animationDir = config.ui.petAnimationDir;
        rendererConfig.frameIntervalOverrideMs = config.ui.animationIntervalMs;
        rendererConfig.scale = config.ui.scale;

        // 序列帧能力不足时降级成静态图片，并说明原因（SRS 6.2）
        if (!caps.imageSequenceLoad.available) {
            DC_WARN(LogCat::Ui,
                    QStringLiteral("Image sequence loading unavailable (%1); "
                                   "falling back to static image")
                        .arg(caps.imageSequenceLoad.reason));
            rendererConfig.animationDir.clear();
        }

        // setConfig 返回成功加载的状态组数量；0 表示只能用内置占位形象。
        //  这不是错误（PET-007 要求绝不能显示加载失败图标），
        //  但要如实告诉用户"没找到形象资源"。
        const int loadedStates = m_renderer->setConfig(rendererConfig);
        if (loadedStates == 0) {
            DC_INFO(LogCat::Ui,
                    QStringLiteral("No pet assets found; using built-in placeholder. "
                                   "Looked under '%1'")
                        .arg(rendererConfig.skinsRoot));

            const QStringList warnings = m_renderer->loadWarnings();
            for (const QString &w : warnings)
                DC_WARN(LogCat::Ui, w);
        }

        // ---- 状态机 ----
        m_stateMachine = new PetStateMachine(this);

        // 把渲染资源的信息灌进状态机配置。
        //  这样"这个状态有几帧、多快"只有一个来源（渲染器），
        //  状态机不自己猜。
        PetStateMachineConfig machineConfig;
        machineConfig.staticImageMode = m_renderer->usingPlaceholder()
                                        && rendererConfig.animationDir.isEmpty();
        m_stateMachine->setConfig(machineConfig);

        // ---- 主窗口 ----
        m_petWindow = new PetWindow(m_renderer, m_stateMachine);

        PetWindowConfig windowConfig;
        windowConfig.alwaysOnTop = config.ui.alwaysOnTop && caps.alwaysOnTop.available;
        windowConfig.opacity = config.ui.opacity;
        windowConfig.scale = config.ui.scale;
        windowConfig.posX = config.ui.posX;
        windowConfig.posY = config.ui.posY;
        windowConfig.startHidden = config.ui.startHidden;
        windowConfig.showStatusHints = config.ui.showStatusHints;
        m_petWindow->setConfig(windowConfig);

        // ---- 气泡 ----
        m_speechBubble = new SpeechBubble;
        m_petWindow->setBubble(m_speechBubble);

        BubbleStyle bubbleStyle;
        bubbleStyle.maxWidth = config.ui.bubbleMaxWidth;
        m_speechBubble->setStyle(bubbleStyle);

        if (!caps.translucentWindow.available) {
            // 不支持半透明时主窗口会显示为不透明矩形，这是可接受的降级。
            //  但不能让用户以为是 bug，所以记一条日志。
            DC_INFO(LogCat::Ui,
                    QStringLiteral("Translucent window unsupported (%1); "
                                   "pet will render with opaque background")
                        .arg(caps.translucentWindow.reason));
        }
    }

    void createInputBox(const AppConfig &config)
    {
        m_inputBox = new CommandInputBox;

        CommandInputConfig inputConfig;
        inputConfig.maxInputLength = config.command.maxInputLength;
        m_inputBox->setConfig(inputConfig);

        // 输入框提交 -> 路由
        connect(m_inputBox, &CommandInputBox::submitted, this,
                [this](const QString &text) { dispatchInteraction(text, QStringLiteral("text")); });
    }

    void createHistoryAndSettings()
    {
        m_historyDialog = new HistoryDialog(m_runtime->history());
        m_settingsDialog = new SettingsDialog(m_runtime);

        // 设置保存后：把新配置应用到界面上（缩放/透明度/语言等）
        connect(m_runtime, &CoreRuntime::configSaved, this,
                [this]() { applyConfigToUi(); });
    }

    void createRecorder()
    {
        m_recorderPanel = new RecorderPanel(m_runtime->recorder(),
                                           m_runtime->history());
    }

    void createTray(const AppConfig &config, const CapabilityReport &caps)
    {
        m_tray = new TrayController(this);

        TrayMenuState state;
        state.trayAvailable = caps.trayIcon.available;
        state.canTogglePet = true;
        state.canOpenInput = true;
        state.canOpenHistory = m_runtime->history() != nullptr;
        state.canOpenSettings = true;

        // 语音：需要服务存在（不是 nullptr）+ 能力就绪 + 用户已启用
        if (m_runtime->speech() && config.speech.enabled) {
            const SpeechServiceStatus speechStatus = m_runtime->speech()->status();
            state.canUseVoice = speechStatus.backendReady && speechStatus.canRecognize;
            if (!state.canUseVoice) {
                state.voiceDisabledReason =
                    speechStatus.limitation.isEmpty()
                        ? I18n::instance().text("settings.speech.no_backend")
                        : speechStatus.limitation;
            }
        } else {
            state.canUseVoice = false;
            state.voiceDisabledReason =
                I18n::instance().text("tray.disabled.speech_off");
        }

        // 录音：同理
        if (m_runtime->recorder() && config.speech.recordingEnabled) {
            QString reason;
            state.canRecord = m_runtime->recorder()->probeCapability(&reason);
            state.recordDisabledReason = reason;
        } else {
            state.canRecord = false;
            state.recordDisabledReason =
                I18n::instance().text("tray.disabled.recording_off");
        }

        // 全局快捷键不可用时的说明
        if (!caps.globalHotkey.available) {
            state.hotkeyDisabledReason = caps.globalHotkey.reason;
        }

        state.petVisibleText = I18n::instance().text("tray.menu.hide_pet");
        state.petImagePath = config.ui.petImagePath;
        state.appName = I18n::instance().text("app.name");

        // 托盘不可用时 initialize 返回 false —— 这不是错误，是降级。
        if (!m_tray->initialize(state)) {
            DC_WARN(LogCat::Ui,
                    QStringLiteral("Tray unavailable, continuing without tray: %1")
                        .arg(m_tray->unavailableReason()));
        }

        if (config.ui.trayIconEnabled && caps.trayIcon.available)
            m_tray->show();
    }

    void createHotkeys(const AppConfig &config)
    {
        m_hotkeys = new HotkeyManager(m_runtime->platform(), this);

        if (!config.hotkey.enabled) {
            DC_INFO(LogCat::Ui,
                    QStringLiteral("Global hotkeys disabled by configuration"));
            return;
        }

        if (!HotkeyManager::isSupported(nullptr)) {
            DC_WARN(LogCat::Ui,
                    QStringLiteral("Global hotkeys unsupported on this platform; "
                                   "tray menu remains the alternative"));
            return;
        }

        QHash<HotkeyAction, QString> configured;
        configured.insert(HotkeyAction::TogglePet, config.hotkey.togglePet);
        configured.insert(HotkeyAction::ToggleMic, config.hotkey.toggleMic);
        configured.insert(HotkeyAction::OpenHistory, config.hotkey.openHistory);
        configured.insert(HotkeyAction::OpenInput, config.hotkey.openInput);

        const HotkeyRegistrationReport report = m_hotkeys->registerAll(configured);

        if (report.failedCount() > 0) {
            // 注册失败要记下来并让用户知道 —— 否则他会反复按一个没生效的键
            //  但也绝不因为热键失败而阻止程序运行（SRS 7.2）
            const QStringList failures = report.failureSummaries();
            for (const QString &f : failures)
                DC_WARN(LogCat::Platform, f);

            if (m_petWindow) {
                m_petWindow->say(
                    I18n::instance().text("app.hotkey.partial_failure",
                                          QStringLiteral("count"),
                                          QString::number(report.failedCount())),
                    8000);
            }
        }
    }

    // -----------------------------------------------------------------------
    // 接线：核心 -> 界面
    // -----------------------------------------------------------------------
    void connectRuntimeSignals()
    {
        IntentRouter *router = m_runtime->router();
        if (!router) {
            DC_ERROR(LogCat::Core,
                     QStringLiteral("No IntentRouter available; interactions disabled"));
            return;
        }

        // ---- 需要授权 ----
        connect(router, &IntentRouter::authorizationRequired, this,
                [this](const dc::PermissionRequest &request) {
                    showPermissionDialog(request);
                });

        // ---- 交互结束 ----
        connect(router, &IntentRouter::interactionFinished, this,
                [this](const dc::InteractionOutcome &outcome) {
                    handleOutcome(outcome);
                });

        // ---- 中间进度 ----
        connect(router, &IntentRouter::interactionProgress, this,
                [this](const QString &taskId, const QString &message) {
                    Q_UNUSED(taskId);
                    if (m_petWindow)
                        m_petWindow->showProgress(message);
                });

        // ---- 启动降级说明 ----
        connect(m_runtime, &CoreRuntime::startupProblemsReported, this,
                [this](const QStringList &problems) {
                    // 不弹模态框（会挡住用户做事），用气泡+托盘说明。
                    if (problems.isEmpty())
                        return;

                    const QString text = I18n::instance().text(
                        "app.startup.degraded",
                        QStringLiteral("count"),
                        QString::number(problems.size()));

                    if (m_petWindow)
                        m_petWindow->say(text, 9000);

                    // 托盘可用时额外给一条更详细的
                    if (m_tray && m_tray->isAvailable()) {
                        m_tray->notify(
                            I18n::instance().text("app.name"),
                            problems.join(QStringLiteral("\n")));
                    }

                    for (const QString &p : problems)
                        DC_WARN(LogCat::Core, p);
                });
    }

    // -----------------------------------------------------------------------
    // 接线：界面 <-> 界面 / 界面 -> 托盘
    // -----------------------------------------------------------------------
    void connectUiSignals()
    {
        // ---- 桌宠窗口 ----
        if (m_petWindow) {
            connect(m_petWindow, &PetWindow::activated, this,
                    [this]() { openInputBox(); });

            connect(m_petWindow, &PetWindow::inputRequested, this,
                    [this]() { openInputBox(); });

            connect(m_petWindow, &PetWindow::historyRequested, this,
                    [this]() { openHistory(); });

            connect(m_petWindow, &PetWindow::settingsRequested, this,
                    [this]() { openSettings(); });

            connect(m_petWindow, &PetWindow::quitRequested, this,
                    &AppController::requestQuit);

            // 拖动后保存位置（写入内存配置，退出时统一落盘，
            //  避免拖动过程中频繁写盘）
            connect(m_petWindow, &PetWindow::positionChanged, this,
                    [this](const QPoint &pos) {
                        AppConfig config = m_runtime->appConfig();
                        if (config.ui.posX == pos.x() && config.ui.posY == pos.y())
                            return;   // 没变就不动，避免无意义的重建

                        config.ui.posX = pos.x();
                        config.ui.posY = pos.y();

                        if (m_runtime->config())
                            m_runtime->config()->setConfig(config);

                        DC_INFO(LogCat::Ui,
                                QStringLiteral("Pet position changed and recorded"));
                    });

            // 右键菜单 -> 复用托盘菜单（同一套动作，不重复实现）
            connect(m_petWindow, &PetWindow::contextMenuRequested, this,
                    [this](const QPoint &globalPos) {
                        if (m_tray && m_tray->isAvailable()) {
                            m_tray->showContextMenu(globalPos);
                        } else {
                            // 没有托盘时退化为打开设置（至少给用户一个入口，
                            //  否则右键点了没反应会让人以为程序卡死了）
                            openSettings();
                        }
                    });
        }

        // ---- 托盘 ----
        if (m_tray) {
            connect(m_tray, &TrayController::togglePetRequested, this, [this]() {
                if (!m_petWindow)
                    return;
                m_petWindow->toggleVisibility();

                // 菜单文案跟着变（"显示桌宠" / "隐藏桌宠"）
                TrayMenuState state = m_tray->menuState();
                state.petVisibleText = m_petWindow->isVisible()
                                           ? I18n::instance().text("tray.menu.hide_pet")
                                           : I18n::instance().text("tray.menu.show_pet");
                m_tray->setMenuState(state);
            });

            connect(m_tray, &TrayController::openInputRequested, this,
                    [this]() { openInputBox(); });

            connect(m_tray, &TrayController::openHistoryRequested, this,
                    [this]() { openHistory(); });

            connect(m_tray, &TrayController::openSettingsRequested, this,
                    [this]() { openSettings(); });

            connect(m_tray, &TrayController::voiceInputRequested, this,
                    [this]() { startVoiceInput(); });

            connect(m_tray, &TrayController::recordToggleRequested, this,
                    [this]() {
                        if (!m_recorderPanel)
                            return;
                        if (m_recorderPanel->isVisible())
                            m_recorderPanel->hide();
                        else
                            m_recorderPanel->open();
                    });

            connect(m_tray, &TrayController::reloadRulesRequested, this,
                    [this]() { openSettings(); });   // 重载入口在设置页

            connect(m_tray, &TrayController::openDataDirectoryRequested, this,
                    [this]() {
                        if (!m_runtime->system())
                            return;

                        const AppPaths paths = m_runtime->paths();
                        // 目录可能还不存在（首次运行），先建出来再打开
                        QDir().mkpath(paths.dataDir);

                        const SystemActionResult r =
                            m_runtime->system()->revealInFileManager(paths.dataDir);
                        if (!r.ok) {
                            DC_WARN(LogCat::System,
                                    QStringLiteral("Failed to open data directory: %1")
                                        .arg(r.message));
                        }
                    });

            connect(m_tray, &TrayController::quitRequested, this,
                    &AppController::requestQuit);
        }

        // ---- 全局快捷键 ----
        if (m_hotkeys) {
            connect(m_hotkeys, &HotkeyManager::hotkeyTriggered, this,
                    [this](dc::HotkeyAction action) {
                        switch (action) {
                        case HotkeyAction::TogglePet:
                            if (m_petWindow)
                                m_petWindow->toggleVisibility();
                            break;
                        case HotkeyAction::ToggleMic:
                            startVoiceInput();
                            break;
                        case HotkeyAction::OpenHistory:
                            openHistory();
                            break;
                        case HotkeyAction::OpenInput:
                            openInputBox();
                            break;
                        case HotkeyAction::Cancel:
                            if (m_inputBox)
                                m_inputBox->dismiss();
                            if (m_petWindow)
                                m_petWindow->hideBubble();
                            break;
                        }
                    });
        }
    }

    // -----------------------------------------------------------------------
    // 动作：全部都是"转发"，没有业务判断
    // -----------------------------------------------------------------------
    void openInputBox()
    {
        if (!m_inputBox)
            return;

        const QRect anchor = m_petWindow ? m_petWindow->anchorRect() : QRect();
        m_inputBox->summon(anchor);
    }

    void openHistory()
    {
        if (m_historyDialog)
            m_historyDialog->open();
    }

    void openSettings()
    {
        if (m_settingsDialog)
            m_settingsDialog->open();
    }

    // 语音输入：SRS 4.5 SPK-002 要求监听状态清晰可见
    void startVoiceInput()
    {
        if (!m_runtime->speech()) {
            if (m_petWindow) {
                m_petWindow->say(
                    I18n::instance().text("app.voice.unavailable"), 5000);
            }
            return;
        }

        const SpeechServiceStatus status = m_runtime->speech()->status();
        if (!status.backendReady || !status.canRecognize) {
            // SRS 6.2：不可用要说明原因，而不是静默失败
            if (m_petWindow) {
                m_petWindow->say(
                    status.limitation.isEmpty()
                        ? I18n::instance().text("settings.speech.no_backend")
                        : status.limitation,
                    7000);
            }
            return;
        }

        // 状态机切到 Listening —— 这是"正在听"的视觉承诺
        if (m_stateMachine)
            m_stateMachine->onListeningStarted();

        if (m_petWindow)
            m_petWindow->say(I18n::instance().text("app.voice.listening"), 0);

        DC_INFO(LogCat::Speech,
                QStringLiteral("Voice input UI requested"));

        // 真正的采集与识别由服务完成，结果通过
        // SpeechService::recognitionFinished 回到这里（见下方连接）。
        //  这里不做任何假设 —— 如果后端是"规则/模拟"实现，
        //  它会走同样的信号回来。
    }

    // 把用户输入交给路由器（唯一的执行入口）
    void dispatchInteraction(const QString &text, const QString &source)
    {
        IntentRouter *router = m_runtime->router();
        if (!router) {
            if (m_petWindow)
                m_petWindow->say(I18n::instance().text("app.error.no_router"), 6000);
            return;
        }

        InteractionRequest request;
        request.rawText = text;
        request.source = source;
        request.language = m_runtime->appConfig().ui.language;

        // 状态机切到 Working（"我正在处理你说的这件事"）
        if (m_stateMachine)
            m_stateMachine->onTaskStarted();

        const InteractionOutcome outcome = router->handle(request);

        // 需要授权的情形：handle 已经提前返回，
        //  真正的完成由 resolveAuthorization 之后的信号触发。
        if (outcome.handled && outcome.taskStatus == TaskStatus::Pending
            && router->hasPendingAuthorization()) {
            return;   // 等用户在权限对话框上做选择
        }

        // 已在同步路径完成的情形（未找到命令、被拒绝等）
        handleOutcome(outcome);
    }

    // -----------------------------------------------------------------------
    // 权限对话框
    // -----------------------------------------------------------------------
    void showPermissionDialog(const PermissionRequest &request)
    {
        PermissionPolicy policy;
        if (m_runtime->permissions())
            policy = m_runtime->permissions()->policy();

        // 每次新建一个对话框。
        //  为什么不像别的窗口那样复用：
        //    权限对话框是"一次性决策"，复用会带来状态残留的风险
        //    （上一次的勾选、下拉选择还在）。安全相关的界面宁可多花一次构造。
        PermissionDialog dialog(request, policy, m_petWindow);

        const PermissionDialog::Result result = dialog.execAndAsk(m_petWindow);

        if (m_petWindow) {
            m_petWindow->say(
                result.accepted()
                    ? I18n::instance().text("app.permission.granted")
                    : I18n::instance().text("app.permission.denied"),
                3000);
        }

        // 把决定交回路由器继续（或终止）执行
        if (m_runtime->router()) {
            const InteractionOutcome outcome =
                m_runtime->router()->resolveAuthorization(result.accepted(),
                                                          result.scope);
            handleOutcome(outcome);
        }
    }

    // -----------------------------------------------------------------------
    // 结果 -> 界面
    // -----------------------------------------------------------------------
    void handleOutcome(const InteractionOutcome &outcome)
    {
        if (!m_petWindow)
            return;

        // ---- 气泡文案 ----
        QString message = outcome.displayMessage;

        // 未命中的情况要把细节也说出来（"我没听懂"要附上为什么）
        if (!outcome.handled) {
            if (message.isEmpty())
                message = I18n::instance().text("app.result.not_understood");
            if (!outcome.detailMessage.isEmpty())
                message += QStringLiteral("\n") + outcome.detailMessage;
        } else if (!outcome.detailMessage.isEmpty()
                   && outcome.taskStatus != TaskStatus::Succeeded) {
            message += QStringLiteral("\n") + outcome.detailMessage;
        }

        if (message.isEmpty())
            message = I18n::instance().text("app.result.done");

        // ---- 歧义时给可点选项（SRS 4.6）----
        if (!outcome.clarificationOptions.isEmpty()) {
            m_petWindow->sayWithChoices(message, outcome.clarificationOptions);
            // 用户选了某个选项后，把它当成新的输入再走一遍
            if (m_speechBubble) {
                connect(m_speechBubble, &SpeechBubble::optionChosen, this,
                        [this](const QString &option) {
                            dispatchInteraction(option, QStringLiteral("clarify"));
                        },
                        Qt::UniqueConnection);
            }
        } else {
            m_petWindow->say(message,
                             m_runtime->appConfig().ui.bubbleDurationMs);
        }

        // ---- 状态机 ----
        if (m_stateMachine) {
            switch (outcome.taskStatus) {
            case TaskStatus::Succeeded:
                // onTaskFinished(true) 会让状态机走"成功 -> 停留 -> 回到说话/待机"
                m_stateMachine->onTaskFinished(true);
                break;
            case TaskStatus::Failed:
            case TaskStatus::TimedOut:
                m_stateMachine->onTaskFinished(false);
                break;
            case TaskStatus::Denied:
            case TaskStatus::Cancelled:
                // 用户自己拒绝/取消的，不算失败 —— 回待机即可，
                //  给个"失败"表情反而像在怪用户。
                m_stateMachine->goIdle();
                break;
            case TaskStatus::Running:
            case TaskStatus::Pending:
                m_stateMachine->onTaskStarted();
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // 配置变化后刷新界面
    // -----------------------------------------------------------------------
    void applyConfigToUi()
    {
        const AppConfig config = m_runtime->appConfig();

        if (m_petWindow) {
            PetWindowConfig windowConfig = m_petWindow->config();
            windowConfig.alwaysOnTop = config.ui.alwaysOnTop;
            windowConfig.opacity = config.ui.opacity;
            windowConfig.scale = config.ui.scale;
            windowConfig.showStatusHints = config.ui.showStatusHints;
            m_petWindow->setConfig(windowConfig);
        }

        if (m_inputBox) {
            CommandInputConfig inputConfig = m_inputBox->config();
            inputConfig.maxInputLength = config.command.maxInputLength;
            m_inputBox->setConfig(inputConfig);
        }

        if (m_speechBubble) {
            BubbleStyle style = m_speechBubble->style();
            style.maxWidth = config.ui.bubbleMaxWidth;
            m_speechBubble->setStyle(style);
        }

        DC_INFO(LogCat::Ui, QStringLiteral("UI refreshed from updated configuration"));
    }

    // -----------------------------------------------------------------------
    // 开机启动同步
    // -----------------------------------------------------------------------
    void syncAutostart()
    {
        if (!m_autostart)
            m_autostart = new AutostartManager(m_runtime->platform(), this);

        const AppConfig config = m_runtime->appConfig();
        const AutostartStatus status = m_autostart->status();

        if (config.startup.enabled && !status.methodAvailable) {
            // 配置要求开机启动但系统不支持：说清楚，不能默默不生效
            DC_WARN(LogCat::Platform,
                    QStringLiteral("Autostart requested but unavailable: %1")
                        .arg(status.unavailableReason));
            return;
        }

        AutostartOptions options;
        options.startMinimized = config.startup.startMinimized;
        options.startHidden = config.ui.startHidden;
        options.delaySeconds = config.startup.delaySeconds;

        if (config.startup.enabled) {
            if (!status.enabled) {
                const Result r = m_autostart->enable(options);
                if (!r.ok) {
                    DC_WARN(LogCat::Platform,
                            QStringLiteral("Failed to enable autostart: %1")
                                .arg(r.message));
                }
            } else if (!status.matchesCurrentExecutable) {
                // 已注册但指向的是旧路径（用户移动了程序）：更新它
                const Result r = m_autostart->update(options);
                if (!r.ok) {
                    DC_WARN(LogCat::Platform,
                            QStringLiteral("Failed to update autostart entry: %1")
                                .arg(r.message));
                }
            }
        } else if (status.enabled) {
            // 配置说不要开机启动，但系统里还有：清掉。
            //  这是 SRS 4.12 明确要求的"关闭时清理启动配置"。
            const Result r = m_autostart->disable();
            if (!r.ok) {
                DC_WARN(LogCat::Platform,
                        QStringLiteral("Failed to disable autostart: %1")
                            .arg(r.message));
            }
        }
    }

    // -----------------------------------------------------------------------
    // 退出
    // -----------------------------------------------------------------------
    void requestQuit()
    {
        DC_INFO(LogCat::Core, QStringLiteral("Quit requested by user"));

        // 退出前检查：还在录音时要问一句。
        //  录音是"用户可能忘了"的典型场景 —— 直接退出会丢数据。
        if (m_runtime->recorder() && m_runtime->recorder()->isRecording()) {
            const auto answer = QMessageBox::question(
                m_petWindow,
                I18n::instance().text("app.name"),
                I18n::instance().text("app.quit.while_recording"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);

            if (answer != QMessageBox::Yes)
                return;

            // 停录音（服务会把文件收尾好）
            m_runtime->recorder()->stop(
                I18n::instance().text("app.quit.stop_reason"));
        }

        if (qApp)
            qApp->quit();
    }

    CoreRuntime *m_runtime = nullptr;

    PetRenderer *m_renderer = nullptr;
    PetStateMachine *m_stateMachine = nullptr;
    PetWindow *m_petWindow = nullptr;
    SpeechBubble *m_speechBubble = nullptr;

    CommandInputBox *m_inputBox = nullptr;
    PermissionDialog *m_permissionDialog = nullptr;   // 每次按需新建，这里仅占位
    HistoryDialog *m_historyDialog = nullptr;
    SettingsDialog *m_settingsDialog = nullptr;
    RecorderPanel *m_recorderPanel = nullptr;

    TrayController *m_tray = nullptr;
    HotkeyManager *m_hotkeys = nullptr;
    AutostartManager *m_autostart = nullptr;
};

} // namespace

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char *argv[])
{
    // ---- 创建 QApplication ----
    //
    //  Qt 6 默认已经处理了高 DPI，不再需要 Qt::AA_EnableHighDpiScaling。
    //  （显式设置反而会产生弃用警告）
    QApplication app(argc, argv);

    // 这些元数据决定了系统里显示的程序名、配置目录名等。
    //  必须在读取配置之前设置好，否则路径会算错。
    QCoreApplication::setOrganizationName(QStringLiteral("DesktopCompanion"));
    QCoreApplication::setApplicationName(QStringLiteral("DesktopCompanion"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(DC_BUILD_APP_VERSION));

    // 最后窗口关闭不退出：桌宠是托盘型应用，
    //  用户关掉设置窗口不代表要退出程序。
    app.setQuitOnLastWindowClosed(false);

    // ---- 命令行参数 ----
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Desktop Companion — lightweight rule-based desktop pet assistant"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption portableOption(
        QStringList{ QStringLiteral("p"), QStringLiteral("portable") },
        QStringLiteral("Portable mode: store all data next to the executable "
                       "(no writes to the user profile)."));
    parser.addOption(portableOption);

    const QCommandLineOption dataDirOption(
        QStringList{ QStringLiteral("d"), QStringLiteral("data-dir") },
        QStringLiteral("Use a specific directory for all data (implicitly portable)."),
        QStringLiteral("path"));
    parser.addOption(dataDirOption);

    const QCommandLineOption hiddenOption(
        QStringLiteral("hidden"),
        QStringLiteral("Start with the pet window hidden (tray only)."));
    parser.addOption(hiddenOption);

    parser.process(app);

    // ---- 决定运行模式 ----
    QString portableDir;
    if (parser.isSet(dataDirOption)) {
        portableDir = parser.value(dataDirOption);
    } else if (parser.isSet(portableOption)) {
        // 便携模式 = 数据放在可执行文件旁边
        portableDir = QCoreApplication::applicationDirPath();
    }

    // ---- 启动核心 ----
    //
    //  用 unique_ptr 而不是栈对象：确保即使后面提前 return，
    //  也能走完整的析构链（配置落盘、数据库关闭等）。
    auto runtime = std::make_unique<CoreRuntime>();

    const StartupReport report = runtime->start(portableDir);

    // 如果连 QApplication 都还没起来就先处理致命错误
    if (!report.ok) {
        // 致命问题（例如数据目录完全不可写）：
        //  这是唯一允许弹出模态框并退出的场景 —— 因为没有界面可用。
        QMessageBox::critical(
            nullptr,
            QStringLiteral("Desktop Companion"),
            report.fatalMessage.isEmpty()
                ? QStringLiteral("Startup failed. See log for details.")
                : report.fatalMessage);
        return 1;
    }

    // ---- 初始化多语言 ----
    //
    //  语言文件放在与应用同级的 resources/i18n/ 下。
    //  加载失败不致命（I18n 会回退到中文，再退到 key 本身）。
    const QString i18nDir = QCoreApplication::applicationDirPath()
                            + QStringLiteral("/resources/i18n");
    const int loadedLanguages = I18n::instance().loadFromDirectory(i18nDir);

    DC_INFO(LogCat::Ui,
            QStringLiteral("Loaded %1 interface language file(s) from %2")
                .arg(loadedLanguages).arg(i18nDir));

    I18n::instance().setLanguage(runtime->appConfig().ui.language);

    // ---- 创建界面 ----
    AppController controller(runtime.get());

    if (!controller.initialize()) {
        // 这里的失败原因必须在日志里留痕。
        // 早先只弹了 QMessageBox，结果 --hidden 或无人值守启动时
        // 用户只看到"程序闪一下退出"，日志里什么都没有，完全无法排查。
        DC_ERROR(LogCat::Ui,
                 QStringLiteral("界面初始化失败，程序退出（退出码 2）。"
                                "若以 --hidden 启动，此错误不会有任何弹窗，"
                                "请检查本行上方的日志定位缺失组件。"));
        QMessageBox::critical(
            nullptr,
            QStringLiteral("Desktop Companion"),
            QStringLiteral("Failed to initialize the user interface. "
                           "Please check the log for details."));
        runtime->shutdown();
        return 2;
    }

    // 命令行要求隐藏启动：覆盖配置（命令行优先于配置，符合直觉）
    if (parser.isSet(hiddenOption)) {
        // 通过改配置对象实现，这样和设置页里的"启动后隐藏"是同一套行为
        AppConfig config = runtime->appConfig();
        config.ui.startHidden = true;
        if (runtime->config())
            runtime->config()->setConfig(config);
    }

    controller.start();

    // ---- 退出前的收尾 ----
    //
    //  aboutToQuit 是唯一可靠的"程序要结束了"信号：
    //  lastWindowClosed 在 quitOnLastWindowClosed=false 时不发，
    //  而析构顺序需要保证在事件循环停止之后、QApplication 析构之前。
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        DC_INFO(LogCat::Core, QStringLiteral("Application about to quit"));

        controller.shutdown();

        // 核心最后关闭：它会落盘配置、关闭数据库、按逆序销毁服务
        runtime->shutdown();
    });

    DC_INFO(LogCat::Core, QStringLiteral("Entering event loop"));

    const int exitCode = app.exec();

    DC_INFO(LogCat::Core,
            QStringLiteral("Event loop exited with code %1").arg(exitCode));

    return exitCode;
}

// AppController 有 Q_OBJECT，需要 moc 处理。
//  把它放在 .cpp 里时必须显式 include 生成的 moc 文件，
//  否则会出现链接错误（undefined reference to vtable）。
#include "main.moc"
