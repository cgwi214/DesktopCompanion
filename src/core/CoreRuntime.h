// ============================================================================
//  CoreRuntime.h — 装配根（composition root）
//
//  需求依据：SRS V1.1 3.2（模块依赖规则）、3.3（核心模块）、5（配置设计）、
//            6.2（能力探测控制功能入口）、7.2（稳定性）
//
//  为什么需要单独的装配层：
//    各服务之间是互相依赖的（搜索要用 SystemService 开浏览器，
//    在线翻译要用 NetworkService 发请求，意图路由要拿到所有服务）。
//    如果让每个服务自己去 new 依赖，会出现三个问题：
//      1. 生命周期失控 —— 谁负责 delete 说不清
//      2. 无法替换 —— 单元测试注入不了假对象
//      3. 配置来源分散 —— 同一个开关在五个地方各读一遍
//    所以这里集中做三件事：创建、按配置注入、在退出时按逆序销毁。
//
//  本文件是**唯一**允许 new 具体服务类的地方。
//  UI 层只持有 CoreRuntime 指针，通过它拿服务，自己不创建任何服务。
//
//  初始化顺序（有依赖关系，不能随意调整）：
//    Logger        —— 其它模块出错时要有地方记
//    → ConfigManager —— 后续一切行为都由配置驱动
//    → PlatformAdapter —— 平台能力探测是所有服务的前提
//    → SystemInfo  —— 能力探测结果决定哪些功能入口开放
//    → PathGuard / PermissionManager / TaskScheduler —— 安全与调度基础
//    → SecureStore —— 密钥存储（翻译后端要用）
//    → HistoryService —— 数据库
//    → CommandRepository / CommandEngine —— 命令规则
//    → SystemService / NetworkService —— 系统操作与网络
//    → SearchService / TranslationService / Speech / Recorder
//    → IntentRouter —— 最后装配，因为它依赖上面全部
// ============================================================================
#pragma once

// 这里直接包含服务头文件而不是前置声明。
//
//  原因：本类的私有方法返回 PathPolicy / NetworkConfigData / SpeechServiceConfig /
//  RecorderConfig / SecureStoreConfig / CommandEngineConfig / PermissionPolicy
//  这些**值类型**。前置声明只能告诉编译器"有这么个名字"，
//  但按值返回/按值传参需要完整定义。
//  与其在后面偷偷补一堆服务头文件，不如在这里显式写清楚依赖 —— 
//  装配根本来就应该知道所有服务的存在。
#include "core/ConfigManager.h"
#include "engine/CommandEngine.h"
#include "engine/PermissionManager.h"
#include "model/Types.h"
#include "platform/PathGuard.h"
#include "platform/SystemInfo.h"
#include "services/NetworkService.h"
#include "services/RecorderService.h"
#include "services/SecureStore.h"
#include "services/SpeechService.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace dc {

// 只做前置声明即可的（本类只持有指针，不按值使用）
class PlatformAdapter;
class TaskScheduler;
class CommandRepository;
class IntentRouter;
class SystemService;
class SearchService;
class TranslationService;
class HistoryService;

// ---------------------------------------------------------------------------
// 启动阶段（用于把「哪一步失败了」准确告诉用户）
// ---------------------------------------------------------------------------
enum class StartupStage {
    NotStarted = 0,
    ReadingConfig,
    ProbingPlatform,
    PreparingStorage,
    LoadingCommandRules,
    InitializingServices,
    Ready,
    Failed
};

QString startupStageToString(StartupStage stage);

// ---------------------------------------------------------------------------
// 启动结果
//
//  设计要点：即使某一步失败，也**尽量**让程序以降级方式继续运行。
//  SRS 6.2 要求「能力缺失要么关闭入口，要么明确说明不可用」，
//  而 SRS 7.2 要求「单个模块失败不得导致核心退出」。
//  所以这里用 startupProblems 收集问题而不是直接失败退出。
// ---------------------------------------------------------------------------
struct StartupReport {
    bool ok = false;                     // 核心功能是否可用（能跑就算 ok）
    StartupStage stage = StartupStage::NotStarted;
    QString fatalMessage;                // 致命问题（如配置目录不可写）
    QStringList startupProblems;         // 非致命问题（功能降级说明）
    QStringList disabledFeatures;        // 因能力不足而未开放的功能
    QString compatibilityReport;         // SystemInfo 生成的能力报告

    bool hasProblems() const { return !startupProblems.isEmpty(); }
};

// ---------------------------------------------------------------------------
// CoreRuntime
// ---------------------------------------------------------------------------
class CoreRuntime : public QObject
{
    Q_OBJECT

public:
    explicit CoreRuntime(QObject *parent = nullptr);
    ~CoreRuntime() override;

    // -----------------------------------------------------------------------
    // 启动
    //
    //  portableDir 非空时表示以「便携模式」运行：所有数据放在
    //  portableDir 下，不写用户目录。这样用户可以放在 U 盘里带走，
    //  也方便开发与测试（不污染真实环境）。
    // -----------------------------------------------------------------------
    StartupReport start(const QString &portableDir = QString());

    // 按逆序销毁全部服务，并落盘配置。
    // 必须在 QApplication 退出前调用，否则配置改动会丢。
    void shutdown();

    bool isReady() const;
    StartupReport report() const;

    // -----------------------------------------------------------------------
    // 服务访问器
    //
    //  约定：start() 之前调用一律返回 nullptr；
    //  调用方必须判空（缺失的服务对应的功能入口不应被打开）。
    // -----------------------------------------------------------------------
    ConfigManager      *config() const      { return m_config; }
    PlatformAdapter    *platform() const    { return m_platform; }
    PermissionManager  *permissions() const { return m_permissions; }
    TaskScheduler      *scheduler() const   { return m_scheduler; }
    CommandRepository  *repository() const  { return m_repository; }
    CommandEngine      *engine() const      { return m_engine; }
    IntentRouter       *router() const      { return m_router; }
    SystemService      *system() const      { return m_system; }
    NetworkService     *network() const     { return m_network; }
    SearchService      *search() const      { return m_search; }
    TranslationService *translation() const { return m_translation; }
    SpeechService      *speech() const      { return m_speech; }
    RecorderService    *recorder() const    { return m_recorder; }
    HistoryService     *history() const     { return m_history; }
    SecureStore        *secureStore() const { return m_secureStore; }

    // 当前配置（start 之后有效；未启动时返回默认构造值）
    AppConfig appConfig() const;

    // 保存配置到磁盘。UI 改设置后调用。
    // 返回 false 时 message 里是面向用户的原因。
    bool saveConfig(QString *message = nullptr);

    // 应用路径布局（供 UI 显示"数据存在哪里"）
    AppPaths paths() const;

    // 能力报告（供"关于/诊断"页面显示）
    CapabilityReport capabilities() const;

    // 环境隔离说明：本程序会用到的、位于项目目录**之外**的位置。
    // 供 ENVIRONMENT_ISOLATION 文档与"关于"页面核对。
    QStringList externalLocations() const;

signals:
    // 启动问题（UI 可弹一次性提示，说明哪些功能被关闭了及原因）
    void startupProblemsReported(const QStringList &problems);

    // 配置变更（UI 各页面据此刷新）
    void configSaved();

private:
    // 各阶段初始化。返回 false 表示该阶段出现致命问题。
    bool initConfig(const QString &portableDir, StartupReport *report);
    bool initPlatform(StartupReport *report);
    bool initSecurity(StartupReport *report);
    bool initHistory(StartupReport *report);
    bool initCommandEngine(StartupReport *report);
    bool initSystemServices(StartupReport *report);
    bool initOptionalServices(StartupReport *report);
    bool initRouter(StartupReport *report);

    // 把 AppConfig 的各个子结构映射到对应服务的配置结构。
    // 集中在这里做，保证"配置字段 → 服务行为"只有一条转换路径。
    void applyConfigToServices();

    // 从配置里解析出各服务的配置对象
    NetworkConfigData     buildNetworkConfig() const;
    SpeechServiceConfig   buildSpeechConfig() const;
    RecorderConfig        buildRecorderConfig() const;
    SecureStoreConfig     buildSecureStoreConfig() const;
    CommandEngineConfig   buildEngineConfig() const;
    PermissionPolicy      buildPermissionPolicy() const;
    PathPolicy            buildPathPolicy() const;
    PrivacySettings       buildPrivacySettings() const;

    // 扫描并加载命令规则文件
    int loadCommandRules(StartupReport *report);

    // 空指针安全检查的辅助
    void addProblem(StartupReport *report, const QString &message);
    void disableFeature(StartupReport *report, const QString &feature,
                        const QString &reason);

    // ---- 服务实例（本类拥有它们的所有权）----
    ConfigManager      *m_config = nullptr;
    PlatformAdapter    *m_platform = nullptr;
    PermissionManager  *m_permissions = nullptr;
    TaskScheduler      *m_scheduler = nullptr;
    CommandRepository  *m_repository = nullptr;
    CommandEngine      *m_engine = nullptr;
    IntentRouter       *m_router = nullptr;
    SystemService      *m_system = nullptr;
    NetworkService     *m_network = nullptr;
    SearchService      *m_search = nullptr;
    TranslationService *m_translation = nullptr;
    SpeechService      *m_speech = nullptr;
    RecorderService    *m_recorder = nullptr;
    HistoryService     *m_history = nullptr;
    SecureStore        *m_secureStore = nullptr;

    StartupReport m_report;
    AppPaths m_paths;
    QString m_portableDir;
    bool m_ready = false;
};

} // namespace dc
