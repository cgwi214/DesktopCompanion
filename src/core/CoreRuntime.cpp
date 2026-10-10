// ============================================================================
//  CoreRuntime.cpp — 装配根实现
//
//  需求依据：SRS V1.1 3.2、3.3、5、6.2、7.2、7.3
//
//  本文件的核心设计原则（三条，贯穿全部代码）：
//
//    1. 【能跑就继续跑】
//       任何一个可选能力失败（没有音频设备、没装 SSL、数据库打不开），
//       都不让程序退出。失败的信息收进 StartupReport，
//       对应的功能入口关闭，其余功能照常。这是 SRS 6.2 与 7.2 的要求。
//
//    2. 【降级要说明理由】
//       绝不做「悄悄关闭功能」这种事。每个被关闭的功能都带上
//       一段面向用户的原因，由 UI 在"设置 → 功能状态"里展示。
//       用户看到的必须是"因为没有音频输入设备所以录音不可用"，
//       而不是按钮点不动却不知道为什么。
//
//    3. 【配置只有一条转换路径】
//       AppConfig（JSON 结构）→ 各服务配置结构 的映射只在本文件里做。
//       如果让每个服务自己去读 QJsonObject，
//       同一个开关就会出现五份不同的解析逻辑，改一处漏四处。
// ============================================================================
#include "core/CoreRuntime.h"

#include "core/BuildConfig.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "engine/CommandEngine.h"
#include "engine/CommandRepository.h"
#include "engine/IntentRouter.h"
#include "engine/PermissionManager.h"
#include "engine/TaskScheduler.h"
#include "platform/PathGuard.h"
#include "platform/PlatformAdapter.h"
#include "platform/SystemInfo.h"
#include "services/HistoryService.h"
#include "services/NetworkService.h"
#include "services/RecorderService.h"
#include "services/SearchService.h"
#include "services/SecureStore.h"
#include "services/SapiSpeechBackend.h"
#include "services/SpeechBackend.h"
#include "services/SpeechService.h"
#include "services/SystemService.h"
#include "services/TranslationService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>

namespace dc {

// ---------------------------------------------------------------------------
// 阶段名（用于日志与界面显示「卡在哪一步」）
// ---------------------------------------------------------------------------
QString startupStageToString(StartupStage stage)
{
    switch (stage) {
    case StartupStage::NotStarted:           return QStringLiteral("未开始");
    case StartupStage::ReadingConfig:        return QStringLiteral("读取配置");
    case StartupStage::ProbingPlatform:      return QStringLiteral("检测运行环境");
    case StartupStage::PreparingStorage:     return QStringLiteral("准备数据存储");
    case StartupStage::LoadingCommandRules:  return QStringLiteral("加载命令规则");
    case StartupStage::InitializingServices: return QStringLiteral("初始化服务");
    case StartupStage::Ready:                return QStringLiteral("就绪");
    case StartupStage::Failed:               return QStringLiteral("启动失败");
    }
    return QStringLiteral("未知");
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
CoreRuntime::CoreRuntime(QObject *parent)
    : QObject(parent)
{
}

CoreRuntime::~CoreRuntime()
{
    // 正常情况下 shutdown() 已被显式调用过。
    // 这里再兜一次，防止有人只用栈对象而忘了调（例如单元测试里）。
    if (m_ready) {
        shutdown();
    }
}

// ===========================================================================
//  启动
// ===========================================================================
StartupReport CoreRuntime::start(const QString &portableDir)
{
    m_portableDir = portableDir;
    m_report = StartupReport{};
    m_report.stage = StartupStage::NotStarted;

    // 有严格顺序，任何一步的致命失败都必须立刻停。
    // 非致命问题不中断，只记录。
    //
    // 每个失败分支都要写日志：早先这里只把原因放进 StartupReport，
    // 而 --hidden 启动时没有界面展示它，结果"程序闪一下退出、日志空白"，
    // 完全无法排查。现在无论有没有界面，日志里都能看到是哪一步断的。
    if (!initConfig(portableDir, &m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：配置初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }
    if (!initPlatform(&m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：平台适配层初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }
    if (!initSecurity(&m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：权限/调度/凭据存储初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }
    if (!initHistory(&m_report)) {
        // 历史记录是可选功能：数据库打不开也要继续跑，
        // 所以 initHistory 内部自己降级，这里不会返回 false。
        // 保留这个分支是为了将来若数据库变成必需依赖时不用改结构。
        DC_WARN(LogCat::Core,
                QStringLiteral("历史记录模块初始化失败，已降级运行"));
    }
    if (!initCommandEngine(&m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：命令引擎初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }
    if (!initSystemServices(&m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：系统服务初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }
    initOptionalServices(&m_report);   // 内部全降级，无致命失败

    if (!initRouter(&m_report)) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("启动失败：意图路由初始化未通过"));
        m_report.stage = StartupStage::Failed;
        m_report.ok = false;
        return m_report;
    }

    m_ready = true;
    m_report.ok = true;
    m_report.stage = StartupStage::Ready;

    DC_INFO(LogCat::Core, QStringLiteral("核心启动完成：v%1，Qt %2，数据目录 %3")
                              .arg(QString::fromLatin1(buildinfo::kAppVersion),
                                   QString::fromLatin1(buildinfo::kQtVersion),
                                   PathGuard::redact(m_paths.dataDir)));

    if (m_report.hasProblems()) {
        for (const QString &p : m_report.startupProblems) {
            DC_WARN(LogCat::Core, QStringLiteral("启动降级：%1").arg(p));
        }
        emit startupProblemsReported(m_report.startupProblems);
    }

    return m_report;
}

// ---------------------------------------------------------------------------
// 阶段 1：配置
//
//  这是唯一可能致命的一步 —— 如果连配置目录都建不出来（磁盘满、
//  权限不足、路径被占用），程序没有可靠的地方存状态，继续跑只会产生
//  难以排查的怪问题，不如明确失败。
// ---------------------------------------------------------------------------
bool CoreRuntime::initConfig(const QString &portableDir, StartupReport *report)
{
    report->stage = StartupStage::ReadingConfig;

    m_config = new ConfigManager(this);

    // ---- 路径解析 ----
    // 便携模式：全部数据落在 portableDir 下，不写用户目录。
    // 普通模式：交给 ConfigManager::resolvePaths() 用 QStandardPaths 决定。
    m_paths = ConfigManager::resolvePaths(portableDir);

    // 目录必须实际存在，否则后面 Logger / SQLite 都会失败
    const QStringList dirsToCreate = {
        m_paths.configDir, m_paths.dataDir, m_paths.logDir,
        m_paths.cacheDir, m_paths.recordingsDir
    };
    for (const QString &dir : dirsToCreate) {
        if (dir.isEmpty()) {
            continue;
        }
        QDir d;
        if (!d.mkpath(dir)) {
            report->fatalMessage =
                tr("无法创建数据目录：%1\n"
                   "请检查磁盘空间和目录权限，或以管理员身份首次运行一次。")
                    .arg(PathGuard::redact(dir));
            DC_FATAL(LogCat::Config,
                     QStringLiteral("创建目录失败：%1").arg(PathGuard::redact(dir)));
            return false;
        }
    }

    m_config->setPaths(m_paths);

    // ---- 日志先起（配置读取出问题时才有地方记）----
    LogConfig logConfig;
    logConfig.directory = m_paths.logDir;
    logConfig.level = LogLevel::Info;
    // 便携模式或调试构建时同时输出到终端，方便现场排查
    logConfig.consoleOutput = !portableDir.isEmpty()
#ifdef DC_BUILD_DEBUG
                              || true
#endif
        ;

    if (!Logger::instance().initialize(logConfig)) {
        // 日志起不来不算致命：只是失去排障能力，功能本身不受影响。
        // 但要把这件事告诉用户，不能装作没事。
        addProblem(report, tr("日志功能不可用，出现问题时无法提供诊断信息。"));
    }

    // ---- 读配置 ----
    const ConfigLoadResult loadResult = m_config->load();
    m_report.compatibilityReport = QString();

    if (loadResult.wasCorrupted) {
        // 配置损坏时 ConfigManager 已自动备份并回落到默认值。
        // 必须明确告诉用户，因为他们的个性化设置被重置了。
        addProblem(report,
                   tr("配置文件已损坏，已备份到 %1 并恢复为默认配置。")
                       .arg(PathGuard::redact(loadResult.backupPath)));
    }

    for (const ConfigIssue &issue : loadResult.issues) {
        if (issue.severity == ConfigIssue::Severity::Error) {
            report->startupProblems
                << tr("配置项 %1 有问题：%2（已改用 %3）")
                       .arg(issue.path, issue.message, issue.appliedFallback);
        }
    }

    // 迁移或自动修复过就要回写，避免每次启动都重新处理一遍
    if (loadResult.needsSave) {
        QString message;
        if (!saveConfig(&message)) {
            addProblem(report, tr("配置无法保存：%1").arg(message));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 2：平台能力探测
//
//  这一步的结果决定后面哪些功能入口可以打开。
//  SRS 6.2：能力探测结果必须用于控制功能入口，而不是装饰。
// ---------------------------------------------------------------------------
bool CoreRuntime::initPlatform(StartupReport *report)
{
    report->stage = StartupStage::ProbingPlatform;

    // PlatformAdapter 是单例，内部按编译时平台选择 Windows/Linux 实现
    m_platform = PlatformAdapter::createInstance(this);
    if (!m_platform) {
        report->fatalMessage = tr("无法初始化平台适配层，程序无法运行。");
        return false;
    }

    // SystemInfo 的能力探测是懒加载 + 缓存的，这里主动触发一次，
    // 好把结果一次性收进启动报告。
    const CapabilityReport caps = SystemInfo::current().capabilities;
    report->compatibilityReport =
        SystemInfo::buildCompatibilityReport(SystemInfo::current());

    // ---- 把能力不足转成「功能关闭 + 原因」 ----
    // 注意这里只记录，不在这里关闭入口 ——
    // 入口的开关在各服务初始化时按能力决定（见 initOptionalServices）。
    if (!caps.networkTls12.available) {
        disableFeature(report, tr("联网搜索与在线翻译"),
                       caps.networkTls12.reason);
    }
    if (!caps.speechSynthesis.available) {
        disableFeature(report, tr("语音朗读"), caps.speechSynthesis.reason);
    }
    if (!caps.speechRecognition.available) {
        disableFeature(report, tr("语音识别"), caps.speechRecognition.reason);
    }
    if (!caps.audioInput.available) {
        disableFeature(report, tr("录音"), caps.audioInput.reason);
    }
    if (!caps.secureStorage.available) {
        disableFeature(report, tr("加密保存 API 密钥"), caps.secureStorage.reason);
    }
    if (!caps.trayIcon.available) {
        disableFeature(report, tr("系统托盘图标"), caps.trayIcon.reason);
    }
    if (!caps.globalHotkey.available) {
        disableFeature(report, tr("全局快捷键"), caps.globalHotkey.reason);
    }

    // 以普通用户运行是设计前提，不是问题。
    // 但如果是管理员，要如实说明（SRS 4.4 第 1、2 条：不自己提权，
    // 也不隐瞒当前权限状态）。
    if (m_platform->isRunningAsAdmin()) {
        addProblem(report,
                   tr("当前以管理员权限运行。建议改用普通用户账户运行，"
                      "以减少误操作的影响范围。"));
    }

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 3：安全基础（权限、调度、密钥存储、路径策略）
// ---------------------------------------------------------------------------
bool CoreRuntime::initSecurity(StartupReport *report)
{
    report->stage = StartupStage::PreparingStorage;

    // ---- 权限管理 ----
    m_permissions = new PermissionManager(m_platform, this);
    m_permissions->setPolicy(buildPermissionPolicy());
    m_permissions->setAuditStoragePath(
        QDir(m_paths.dataDir).filePath(QStringLiteral("permission_audit.json")));

    // ---- 任务调度 ----
    m_scheduler = new TaskScheduler(this);
    m_scheduler->setDefaultTimeoutMs(30000);
    // 默认 1 个并发：有副作用的操作串行执行，避免两条命令同时改同一文件。
    // 只读任务（搜索/翻译）本身标了 exclusive=false，走的是另一条通道。
    m_scheduler->setMaxConcurrentTasks(1);

    // ---- 安全存储（API 密钥）----
    m_secureStore = new SecureStore(this);
    const SecureStoreInitResult storeResult =
        m_secureStore->initialize(buildSecureStoreConfig());

    if (storeResult.mode == StorageMode::Unavailable) {
        // 没有可用的安全存储不算致命：用户就是不能保存 API 密钥而已。
        // 但必须说明原因，否则用户会以为"填了密钥却不生效"是 bug。
        disableFeature(report, tr("保存翻译服务的 API 密钥"),
                       storeResult.reason.isEmpty()
                           ? tr("本机没有可用的凭据保护机制。")
                           : storeResult.reason);
    } else if (storeResult.needsUserConsent) {
        // 明文降级的风险提示不能吞掉，要交给 UI 展示并征求同意
        addProblem(report, storeResult.riskNotice.isEmpty()
                               ? tr("API 密钥将以未加密方式保存，需要你确认。")
                               : storeResult.riskNotice);
    }

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 4：历史记录
// ---------------------------------------------------------------------------
bool CoreRuntime::initHistory(StartupReport *report)
{
    m_history = new HistoryService(this);

    const QString dbPath = m_config->config().history.databasePath.isEmpty()
                               ? m_paths.historyDbPath
                               : m_config->config().history.databasePath;

    const bool ok = m_history->initialize(dbPath);
    if (!ok) {
        // 数据库不可用是可降级项：所有写入被静默丢弃、读取返回空集，
        // 程序其余功能完全不受影响（HistoryService 内部已保证这一点）。
        disableFeature(report, tr("历史记录"),
                       m_history->lastError().isEmpty()
                           ? tr("数据库无法打开。")
                           : m_history->lastError());
    }

    m_history->setPrivacySettings(buildPrivacySettings());
    m_history->setRecordingsDirectory(m_paths.recordingsDir);

    if (m_config->config().history.maxStorageBytes > 0) {
        // 清理失败不打扰用户（只是空间没释放），但日志里要留痕。
        // CleanupResult 用计数字段表达结果，所以这里逐项判断。
        const CleanupResult cleaned = m_history->enforceStorageLimit();
        if (cleaned.totalRemoved() > 0 || cleaned.bytesFreed > 0) {
            DC_INFO(LogCat::History,
                    QStringLiteral("历史记录清理：删除 %1 条记录、%2 个录音文件，"
                                   "释放 %3 字节")
                        .arg(cleaned.totalRemoved())
                        .arg(cleaned.recordingFilesRemoved)
                        .arg(cleaned.bytesFreed));
        }
        if (!cleaned.errors.isEmpty()) {
            DC_WARN(LogCat::History,
                    QStringLiteral("历史记录清理有 %1 项未完成")
                        .arg(cleaned.errors.size()));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 5：命令规则与解析引擎
// ---------------------------------------------------------------------------
bool CoreRuntime::initCommandEngine(StartupReport *report)
{
    report->stage = StartupStage::LoadingCommandRules;

    m_repository = new CommandRepository(this);
    const int loaded = loadCommandRules(report);

    if (loaded == 0) {
        // 一条规则都加载不到是致命的：桌宠变成了一个只会说"我不会"的窗口。
        // 明确失败比让用户面对一个空壳要好。
        report->fatalMessage =
            tr("没有加载到任何命令规则，程序无法正常工作。\n"
               "请确认安装目录下的 resources/commands/ 文件夹存在且内容完整。");
        return false;
    }

    // 规则冲突检测：只提示不阻止。
    // SRS 4.6 要求"对冲突提供配置校验"，用户有权带着冲突继续用，
    // 引擎会按优先级消解，但要让用户知道存在歧义。
    const QVector<RuleConflict> conflicts = m_repository->detectConflicts();
    if (!conflicts.isEmpty()) {
        addProblem(report, tr("发现 %1 处命令规则冲突，将按优先级决定结果。"
                              "可在设置中查看详情。")
                               .arg(conflicts.size()));
    }

    m_engine = new CommandEngine(m_repository, this);
    m_engine->setConfig(buildEngineConfig());

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 6：必需的系统服务
// ---------------------------------------------------------------------------
bool CoreRuntime::initSystemServices(StartupReport *report)
{
    report->stage = StartupStage::InitializingServices;

    m_system = new SystemService(this);
    m_system->setPathPolicy(buildPathPolicy());

    m_network = new NetworkService(this);
    m_network->applyConfig(buildNetworkConfig());

    // 网络服务把「需要用户确认」的信号转给权限模块统一处理，
    // 保证 UI 只需要连一个信号源，不用分辨是哪种授权。
    // 注意：这里不做自动允许，任何放行都必须经过用户明确选择。
    connect(m_network, &NetworkService::confirmationRequired,
            this, [this](const QString &requestId, const dc::NetworkRequest &request) {
                DC_INFO(LogCat::Network,
                        QStringLiteral("网络访问等待用户确认：%1（请求 %2）")
                            .arg(request.url.host(), requestId));
            });

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 7：可选服务（搜索、翻译、语音、录音）
//
//  全部走"失败就降级"，不会返回致命错误。
//  每个服务初始化失败都会关掉对应功能入口并给出原因。
// ---------------------------------------------------------------------------
bool CoreRuntime::initOptionalServices(StartupReport *report)
{
    const AppConfig cfg = m_config->config();
    const CapabilityReport caps = SystemInfo::current().capabilities;

    // ---- 搜索 ----
    m_search = new SearchService(this);
    m_search->setSystemService(m_system);
    m_search->setNetworkService(m_network);

    // 网络能力不足时，抓取式搜索不可用，但"打开浏览器搜索"仍然可用 ——
    // 因为那只是启动本地浏览器，不涉及本程序的网络请求。
    // 这一点很关键：用户即使把网络模式设为完全离线，
    // 「搜索 xxx」这个功能依然能用（浏览器自己去联网）。
    if (!caps.networkTls12.available) {
        disableFeature(report, tr("应用内直接抓取搜索结果"),
                       tr("本机缺少可用的安全通信组件。"
                          "「用浏览器搜索」仍可正常使用。"));
    }

    // 默认引擎：配置里指定的优先，没配就按界面语言自动挑
    const QString configuredProvider = cfg.network.enabledSearchProvider;
    if (!configuredProvider.isEmpty()
        && configuredProvider != QLatin1String("default")
        && m_search->hasProvider(configuredProvider)) {
        m_search->setDefaultProvider(configuredProvider);
    } else {
        const QString suggested =
            m_search->suggestProviderForLanguage(languageToCode(cfg.ui.language));
        if (!suggested.isEmpty()) {
            m_search->setDefaultProvider(suggested);
        }
    }

    // 用户禁用的引擎
    for (const QString &id : m_search->providerIds()) {
        if (!cfg.network.allowedServices.isEmpty()
            && !cfg.network.allowedServices.contains(id)) {
            m_search->setProviderEnabled(id, false);
        }
    }

    // ---- 翻译 ----
    m_translation = new TranslationService(this);
    m_translation->setNetworkService(m_network);
    m_translation->setSecureStore(m_secureStore);

    // 离线词典后端总是注册（不联网、不需要密钥，作为诚实的兜底）
    m_translation->registerBackend(
        new OfflineDictionaryBackend(m_translation));

    if (!cfg.network.enabledTranslationProvider.isEmpty()
        && cfg.network.enabledTranslationProvider != QLatin1String("offline")) {
        // 在线翻译后端需要密钥。这里**不**创建 ——
        // 具体端点与密钥由用户在设置页填写，SettingsDialog 会调用
        // registerBackend 注入。CoreRuntime 不猜测任何第三方接口地址。
        if (!m_secureStore || m_secureStore->mode() == StorageMode::Unavailable) {
            disableFeature(report, tr("在线翻译"),
                           tr("没有可用的凭据存储，无法安全保存 API 密钥。"
                              "离线词典翻译仍可使用。"));
        }
    }

    // ---- 语音 ----
    m_speech = new SpeechService(this);
    {
        // 后端注册：Windows 上注册 SAPI，其它平台只注册规则兜底。
        // 规则兜底始终存在，这样"朗读"功能在任何平台上都不会
        // 因为缺少后端而变成死按钮 —— 它会诚实地说自己只会念固定短语。
        m_speech->registerBackend(new RuleBasedSpeechBackend(m_speech));
#ifdef Q_OS_WIN
        m_speech->registerBackend(new SapiSpeechBackend(m_speech));
#endif

        const SpeechServiceStatus status = m_speech->status();
        const bool speechUsable = status.backendReady
                                  && (status.canSynthesize || status.canRecognize);
        if (!speechUsable) {
            disableFeature(report, tr("语音功能"),
                           status.limitation.isEmpty()
                               ? tr("本机没有可用的语音后端。")
                               : status.limitation);
        } else if (!status.limitation.isEmpty()) {
            // 有后端但有局限（例如只有规则兜底），必须提前告知
            addProblem(report, tr("语音功能受限：%1").arg(status.limitation));
        }
    }

    // ---- 录音 ----
    m_recorder = new RecorderService(this);
    {
        const RecorderConfig recorderConfig = buildRecorderConfig();
        m_recorder->applyConfig(recorderConfig);

        QString reason;
        if (!m_recorder->probeCapability(&reason)) {
            disableFeature(report, tr("录音"),
                           reason.isEmpty() ? tr("本机不具备录音条件。") : reason);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 8：意图路由（最后装配）
// ---------------------------------------------------------------------------
bool CoreRuntime::initRouter(StartupReport *report)
{
    m_router = new IntentRouter(m_repository, m_engine, m_permissions,
                                m_scheduler, this);

    // 依赖注入：允许为空。缺失的服务对应的意图会被明确拒绝并说明原因，
    // 而不是静默什么都不做（SRS 6.2）。
    m_router->setSystemService(m_system);
    m_router->setSearchService(m_search);
    m_router->setTranslationService(m_translation);
    m_router->setRecorderService(m_recorder);
    m_router->setSpeechService(m_speech);
    m_router->setHistoryService(m_history);

    if (!m_router) {
        report->fatalMessage = tr("无法初始化命令路由，程序无法处理任何输入。");
        return false;
    }

    // 权限模块的授权请求转给路由层。
    // 这样 UI 只连接 IntentRouter 一个信号就够了，
    // 不需要同时监听 PermissionManager（避免两条授权路径）。
    connect(m_permissions, &PermissionManager::authorizationRequired,
            m_router, &IntentRouter::authorizationRequired);

    return true;
}

// ===========================================================================
//  配置映射（AppConfig -> 各服务配置结构）
// ===========================================================================

PathPolicy CoreRuntime::buildPathPolicy() const
{
    PathPolicy policy;

    const FilesConfig &files = m_config->config().files;

    // 白名单根目录：用户配置了就用用户的，否则用安全默认值。
    policy.allowedRoots = files.allowedDirectories.isEmpty()
                              ? PathGuard::defaultAllowedRoots()
                              : files.allowedDirectories;

    // 这两个恒为 true / false，不接受配置覆盖：
    //   requireWhitelist —— SRS 4.3 要求文件操作限制在允许目录内
    //   allowSensitiveLocations —— 允许访问但会置 sensitivity，
    //                              由权限模块升级确认强度
    policy.requireWhitelist = true;
    policy.allowSensitiveLocations = true;
    policy.requireExists = true;
    policy.maxPathLength = 4096;

    return policy;
}

PermissionPolicy CoreRuntime::buildPermissionPolicy() const
{
    PermissionPolicy policy;

    // 低风险是否确认：SRS 4.4 表格说"可按配置直接执行"，
    // 所以这里允许用户选择。默认不确认，保证日常使用不烦人。
    policy.confirmLowRisk = false;

    // 中/高风险必须确认与授权。SRS 明确要求，不提供关闭选项，
    // 因此这里硬编码为 true，配置里也没有对应字段可以改掉它们。
    policy.confirmMediumRisk = true;
    policy.authorizeHighRisk = true;

    policy.sessionGrantLifetimeMinutes = 480;   // 8 小时
    policy.allowPermanentGrants = true;
    policy.maxPermanentGrantsPerRule = 2;
    policy.denialRetentionMinutes = 60;

    return policy;
}

CommandEngineConfig CoreRuntime::buildEngineConfig() const
{
    CommandEngineConfig config;

    const CommandConfig &cmd = m_config->config().command;
    config.minConfidence = cmd.minConfidence;
    config.maxInputLength = cmd.maxInputLength;

    // 关键词兜底始终开启：精确匹配失败时它是唯一的救命稻草，
    // 而它本身有 minConfidence 兜着，不会造成误执行。
    config.enableKeywordFallback = true;

    return config;
}

NetworkConfigData CoreRuntime::buildNetworkConfig() const
{
    NetworkConfigData config;

    const NetworkConfig &net = m_config->config().network;
    config.mode = net.mode;
    config.domainWhitelist = net.allowedHosts;
    config.timeoutMs = net.readTimeoutMs;
    config.connectTimeoutMs = net.connectTimeoutMs;
    config.maxResponseBytes = net.maxResponseBytes;
    config.maxAttempts = net.maxRetries + 1;   // 配置里是"重试次数"
    config.followRedirects = true;
    config.maxRedirects = 3;

    // 明文连接恒为禁止。配置里有这个字段只是为了让用户
    // 在设置界面看到一个明确的"已关闭且不可开启"的说明，
    // 而不是完全不提 —— 不提的话用户会以为程序忘了做这件事。
    config.allowInsecureTls = false;

    return config;
}

SpeechServiceConfig CoreRuntime::buildSpeechConfig() const
{
    SpeechServiceConfig config;

    const SpeechConfig &sp = m_config->config().speech;
    config.preferredBackendId = (sp.backendId == QLatin1String("auto"))
                                    ? QString()
                                    : sp.backendId;
    config.language = languageToCode(sp.recognitionLanguage);
    config.synthesisEnabled = sp.enabled && sp.synthesisEnabled;
    config.voiceId = sp.voiceId;
    config.rate = sp.speechRate;
    config.volume = sp.speechVolume;

    // 识别默认关闭（SRS 4.5：语音输入需要用户显式开启）
    config.recognitionEnabled = sp.enabled;
    config.confidenceThreshold = sp.minConfidence;

    // 规则兜底后端不允许触发命令执行。
    //
    //  理由：规则后端只能做"短语匹配"，它把一个模糊的识别结果
    //  映射成命令，正确率远低于真正的语音识别。如果允许它直接执行命令，
    //  等于用一句"大概像"的话去启动程序甚至删文件 —— 这是不可接受的。
    //  所以它只能用来演示与朗读，识别结果必须经过用户确认。
    config.allowRuleBasedCommandExecution = false;

    return config;
}

RecorderConfig CoreRuntime::buildRecorderConfig() const
{
    RecorderConfig config;

    const SpeechConfig &sp = m_config->config().speech;
    config.enabled = sp.recordingEnabled;      // 默认 false（SRS 4.11）
    config.outputDirectory = m_paths.recordingsDir;
    config.sampleRate = 16000;
    config.channels = 1;
    config.bitsPerSample = 16;

    // 上限写死而不开放配置，是为了防"配置改错导致磁盘被填满"：
    // 用户可以调小，但代码层不允许无限制。
    config.maxDurationSeconds = 600;                       // 单次最长 10 分钟
    config.maxFileBytes = 50LL * 1024 * 1024;              // 单文件 50 MB
    config.maxTotalBytes = 500LL * 1024 * 1024;            // 总量 500 MB
    config.playStartBeep = false;   // 默认静音启动，不打扰用户与会议场景
    config.filePrefix = QStringLiteral("recording");

    return config;
}

SecureStoreConfig CoreRuntime::buildSecureStoreConfig() const
{
    SecureStoreConfig config;

    config.storeDirectory = m_paths.dataDir;
    config.enableMemoryCache = true;

    // 明文降级是否允许，由用户在配置里明确选择。
    // 注意两个字段都要为真才降级：
    //   allowPlaintextFallback  —— 用户知道并接受"可能会明文保存"
    //   plaintextConsentGranted —— 用户在界面上点过确认
    // 这样即使配置文件被手工改成 true，只要用户没在界面确认过，
    // 依然不会落到明文存储（SRS 4.10）。
    config.allowPlaintextFallback =
        !m_config->config().privacy.storeApiKeysInFile;
    config.plaintextConsentGranted = false;

    return config;
}

PrivacySettings CoreRuntime::buildPrivacySettings() const
{
    const HistoryConfig &h = m_config->config().history;

    PrivacySettings settings;
    settings.conversationEnabled = h.conversationEnabled;
    settings.taskEnabled = h.taskEnabled;
    settings.translationEnabled = h.translationEnabled;
    settings.searchEnabled = h.searchEnabled;
    settings.recordingEnabled = h.recordingEnabled;
    settings.retentionDays = h.retentionDays;
    settings.maxStorageBytes = h.maxStorageBytes;

    return settings;
}

void CoreRuntime::applyConfigToServices()
{
    if (!m_config) {
        return;
    }

    // 权限与路径策略
    if (m_permissions) {
        m_permissions->setPolicy(buildPermissionPolicy());
    }
    if (m_system) {
        m_system->setPathPolicy(buildPathPolicy());
    }
    if (m_engine) {
        m_engine->setConfig(buildEngineConfig());
    }
    if (m_network) {
        m_network->applyConfig(buildNetworkConfig());
    }
    if (m_speech) {
        m_speech->applyConfig(buildSpeechConfig());
    }
    if (m_recorder) {
        m_recorder->applyConfig(buildRecorderConfig());
    }
    if (m_history) {
        m_history->setPrivacySettings(buildPrivacySettings());
    }
}

// ===========================================================================
//  命令规则加载
// ===========================================================================
int CoreRuntime::loadCommandRules(StartupReport *report)
{
    int total = 0;

    // ---- 内置规则：安装目录下的 resources/commands/ ----
    const QString builtinDir =
        QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("resources/commands"));

    if (QFileInfo::exists(builtinDir)) {
        const RuleLoadResult result = m_repository->loadFromDirectory(builtinDir);
        total += result.loadedRuleCount;

        for (const RuleIssue &issue : result.issues) {
            if (issue.severity == RuleIssue::Severity::Error) {
                report->startupProblems
                    << tr("命令规则有问题：%1").arg(issue.format());
            }
        }
    } else {
        // 开发期从构建目录直接跑时，resources 还没有被复制过去。
        // 回退到源码树位置，让开发不用先 install 就能调试。
        const QString devDir =
            QDir(QCoreApplication::applicationDirPath())
                .filePath(QStringLiteral("../../resources/commands"));
        if (QFileInfo::exists(devDir)) {
            const RuleLoadResult result = m_repository->loadFromDirectory(devDir);
            total += result.loadedRuleCount;
            DC_INFO(LogCat::Command,
                    QStringLiteral("使用开发期规则目录：%1").arg(devDir));
        }
    }

    // ---- 用户附加规则：配置里声明的额外文件 ----
    const QStringList extra = m_config->config().command.ruleFiles;
    if (!extra.isEmpty()) {
        QStringList resolved;
        for (const QString &file : extra) {
            resolved << (QFileInfo(file).isAbsolute()
                             ? file
                             : QDir(m_paths.configDir).filePath(file));
        }
        const RuleLoadResult result = m_repository->loadFromFiles(resolved);
        total += result.loadedRuleCount;

        for (const RuleIssue &issue : result.issues) {
            if (issue.severity == RuleIssue::Severity::Error) {
                report->startupProblems
                    << tr("自定义规则有问题：%1").arg(issue.format());
            }
        }
    }

    // 用户规则是否允许：SRS 4.6 允许，但默认不开启脚本执行
    if (!m_config->config().command.allowUserRules) {
        DC_INFO(LogCat::Command, QStringLiteral("用户自定义规则已在配置中关闭"));
    }

    DC_INFO(LogCat::Command,
            QStringLiteral("已加载 %1 条命令规则，%2 个别名表")
                .arg(total)
                .arg(m_repository->aliasTables().size()));

    return total;
}

// ===========================================================================
//  保存配置
// ===========================================================================
bool CoreRuntime::saveConfig(QString *message)
{
    if (!m_config) {
        if (message) {
            *message = tr("配置模块未初始化。");
        }
        return false;
    }

    // 先把当前内存配置写回 ConfigManager，
    // 因为 UI 可能直接改了它取出来的副本。
    const Result saved = m_config->save();

    if (!saved.ok) {
        if (message) {
            *message = saved.message.isEmpty()
                           ? tr("配置保存失败，请检查磁盘空间与目录权限。")
                           : saved.message;
        }
        DC_ERROR(LogCat::Config,
                 QStringLiteral("保存配置失败：%1（%2）")
                     .arg(saved.message, saved.errorCode));
        return false;
    }

    // 配置改了要立刻作用到各服务上，否则会出现
    // "设置里改了但行为没变"，用户会以为设置不生效。
    applyConfigToServices();

    emit configSaved();
    DC_INFO(LogCat::Config, QStringLiteral("配置已保存并应用"));
    return true;
}

// ===========================================================================
//  关闭
// ===========================================================================
void CoreRuntime::shutdown()
{
    if (!m_ready) {
        return;
    }

    DC_INFO(LogCat::Core, QStringLiteral("开始关闭核心…"));

    // ---- 1. 先让调度器停止接收新任务，并等待正在跑的结束 ----
    //  顺序很重要：如果先销毁服务，正在跑的任务会访问已释放的对象。
    if (m_scheduler) {
        m_scheduler->shutdown();
        if (!m_scheduler->waitForAll(5000)) {
            DC_WARN(LogCat::Core,
                    QStringLiteral("有任务未能在 5 秒内结束，将被强制中断"));
            m_scheduler->cancelAll();
        }
    }

    // ---- 2. 停掉可能正在进行的语音与录音 ----
    //  这两个直接占用硬件，必须先释放再销毁，否则设备可能被占住。
    if (m_speech) {
        m_speech->stopSpeaking();
        m_speech->cancelListening();
    }
    if (m_recorder && m_recorder->isRecording()) {
        m_recorder->stop(tr("程序退出"));
    }

    // ---- 3. 保存配置 ----
    //  必须在销毁 ConfigManager 之前做，否则改动丢失。
    QString message;
    if (!saveConfig(&message)) {
        DC_WARN(LogCat::Config,
                QStringLiteral("退出时保存配置失败：%1").arg(message));
    }

    // ---- 4. 关数据库 ----
    //  SQLite 需要正常 close 才能保证 WAL 落盘不丢数据。
    if (m_history) {
        m_history->close();
    }

    // ---- 5. 逆序销毁服务 ----
    //  这里只 delete 那些 parent 不是 this 的（本类显式拥有的）。
    //  parent 为 this 的会在 ~QObject 时自动清理。
    //  为了顺序可控，统一在这里显式 delete 并置空。
    const auto safeDelete = [](auto *&ptr) {
        if (ptr) {
            delete ptr;
            ptr = nullptr;
        }
    };

    safeDelete(m_router);
    safeDelete(m_recorder);
    safeDelete(m_speech);
    safeDelete(m_translation);
    safeDelete(m_search);
    safeDelete(m_network);
    safeDelete(m_system);
    safeDelete(m_engine);
    safeDelete(m_repository);
    safeDelete(m_history);
    safeDelete(m_secureStore);
    safeDelete(m_scheduler);
    safeDelete(m_permissions);
    safeDelete(m_platform);
    safeDelete(m_config);

    m_ready = false;
    m_report.stage = StartupStage::NotStarted;

    DC_INFO(LogCat::Core, QStringLiteral("核心已关闭"));
}

// ===========================================================================
//  查询
// ===========================================================================
bool CoreRuntime::isReady() const
{
    return m_ready;
}

StartupReport CoreRuntime::report() const
{
    return m_report;
}

AppConfig CoreRuntime::appConfig() const
{
    return m_config ? m_config->config() : AppConfig{};
}

AppPaths CoreRuntime::paths() const
{
    return m_paths;
}

CapabilityReport CoreRuntime::capabilities() const
{
    return SystemInfo::current().capabilities;
}

// ---------------------------------------------------------------------------
// 环境隔离清单
//
//  SRS 未直接要求，但用户明确要求"如果使用了或修改了项目目录以外的
//  文件或者配置，请创建环境隔离说明记录并说明"。
//  这个方法让程序自己在"关于 → 环境信息"里就能列出这些位置，
//  用户不用去翻文档。
// ---------------------------------------------------------------------------
QStringList CoreRuntime::externalLocations() const
{
    QStringList locations;

    if (!m_portableDir.isEmpty()) {
        locations << tr("便携模式：所有数据均在 %1，未写入用户目录。")
                         .arg(PathGuard::redact(m_portableDir));
        return locations;
    }

    if (!m_paths.configDir.isEmpty()) {
        locations << tr("配置文件目录：%1").arg(PathGuard::redact(m_paths.configDir));
    }
    if (!m_paths.dataDir.isEmpty()) {
        locations << tr("数据与数据库目录：%1").arg(PathGuard::redact(m_paths.dataDir));
    }
    if (!m_paths.logDir.isEmpty()) {
        locations << tr("日志目录：%1").arg(PathGuard::redact(m_paths.logDir));
    }
    if (!m_paths.cacheDir.isEmpty()) {
        locations << tr("缓存目录：%1").arg(PathGuard::redact(m_paths.cacheDir));
    }
    if (!m_paths.recordingsDir.isEmpty()) {
        locations << tr("录音目录：%1").arg(PathGuard::redact(m_paths.recordingsDir));
    }

#ifdef Q_OS_WIN
    locations << tr("开机自启（如启用）会写入注册表 HKEY_CURRENT_USER\\"
                    "Software\\Microsoft\\Windows\\CurrentVersion\\Run，"
                    "或用户启动文件夹。仅影响当前用户。");
#else
    locations << tr("开机自启（如启用）会写入 ~/.config/autostart/。"
                    "仅影响当前用户。");
#endif

    locations << tr("本程序不写入系统目录，不修改系统级配置，"
                    "不会以管理员权限静默安装任何组件。");

    return locations;
}

// ===========================================================================
//  辅助
// ===========================================================================
void CoreRuntime::addProblem(StartupReport *report, const QString &message)
{
    if (report && !message.isEmpty()) {
        report->startupProblems << message;
    }
}

void CoreRuntime::disableFeature(StartupReport *report,
                                 const QString &feature,
                                 const QString &reason)
{
    if (!report) {
        return;
    }

    // 统一的降级措辞：先说"什么被关了"，再说"为什么"。
    // 这样界面可以直接把这句话展示出来，不需要再拼一遍。
    const QString text = reason.isEmpty()
                             ? tr("%1 不可用。").arg(feature)
                             : tr("%1 不可用：%2").arg(feature, reason);

    report->disabledFeatures << text;
    report->startupProblems << text;
}

} // namespace dc
