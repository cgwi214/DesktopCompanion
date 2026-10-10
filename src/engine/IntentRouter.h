// ============================================================================
//  IntentRouter.h — 意图路由（把解析结果分派到具体服务）
//
//  需求依据：SRS V1.1 第 3.2 节（模块依赖规则）
//    - UI 层不得直接执行系统命令或访问网络
//    - 所有系统操作统一通过 SystemService
//    - 所有联网请求统一经过 NetworkService 的网络权限检查
//    - 所有高风险操作统一经过 PermissionManager
//    - 语音识别结果必须进入 CommandEngine，不得直接执行系统命令
//
//  本类就是这些规则的「执行者」：它是唯一知道
//    「intent -> 哪个服务 -> 需要什么权限」
//  的地方。UI 层把用户输入交给它，它负责完整链路。
//
//  执行链路：
//    1. CommandEngine.parse()        -> 得到意图与参数
//    2. PermissionManager.check()    -> 权限判定
//    3. [需要授权时] 弹窗等待用户决策
//    4. TaskScheduler.submit()       -> 异步执行
//    5. 结果汇总 -> 写历史 -> 气泡反馈 -> 审计
// ============================================================================
#pragma once

#include "engine/CommandEngine.h"
#include "engine/PermissionManager.h"
#include "engine/TaskScheduler.h"
#include "model/Types.h"

#include <QObject>
#include <QString>
#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QStringList>

namespace dc {

class HistoryService;
class SystemService;
class SearchService;
class TranslationService;
class RecorderService;
class SpeechService;
class CommandRepository;

// ---------------------------------------------------------------------------
// 一次交互的完整请求
// ---------------------------------------------------------------------------
struct InteractionRequest {
    QString rawText;                    // 用户原始输入
    Language language = Language::Auto; // 指定语言
    QString source;                     // "text" / "voice" / "hotkey"
    bool skipConfirmation = false;      // 仅用于「用户已在界面上确认」的内部路径
};

// ---------------------------------------------------------------------------
// 交互结果（供 UI 渲染气泡 + 写历史）
// ---------------------------------------------------------------------------
struct InteractionOutcome {
    bool handled = false;               // 是否找到了可执行的命令
    bool executed = false;              // 是否真的执行了
    bool cancelled = false;             // 用户取消

    QString ruleId;
    QString intent;
    QString taskId;

    TaskStatus taskStatus = TaskStatus::Pending;
    QString displayMessage;             // 气泡文字（面向用户）
    QString detailMessage;              // 补充说明（错误原因等）
    QString errorCode;

    ParseOutcome parseOutcome = ParseOutcome::NoMatch;
    QStringList clarificationOptions;   // 歧义时的选项文本

    qint64 elapsedMs = 0;
};

// ---------------------------------------------------------------------------
// IntentRouter
// ---------------------------------------------------------------------------
class IntentRouter : public QObject
{
    Q_OBJECT

public:
    IntentRouter(CommandRepository *repository,
                 CommandEngine *engine,
                 PermissionManager *permissions,
                 TaskScheduler *scheduler,
                 QObject *parent = nullptr);
    ~IntentRouter() override;

    // 注入服务（允许为空 —— 缺失的服务对应的意图会被明确拒绝并说明原因，
    // 这样在「语音模块未编译」等降级场景下仍能正常运行，SRS 6.2）
    void setSystemService(SystemService *service);
    void setSearchService(SearchService *service);
    void setTranslationService(TranslationService *service);
    void setRecorderService(RecorderService *service);
    void setSpeechService(SpeechService *service);
    void setHistoryService(HistoryService *service);

    // -----------------------------------------------------------------------
    // 主入口：处理一次用户交互
    //
    //  完整执行「解析 -> 权限 -> 执行 -> 记录」链路。
    //  非阻塞：需要授权的会先返回，由 UI 调用 resolveAuthorization() 继续。
    // -----------------------------------------------------------------------
    InteractionOutcome handle(const InteractionRequest &request);

    // -----------------------------------------------------------------------
    // 授权后继续执行
    //
    //  用户在授权对话框上做出选择后调用。
    //  granted=false 时会写入拒绝记录（SRS 4.4 第 8 条）。
    // -----------------------------------------------------------------------
    InteractionOutcome resolveAuthorization(bool granted, GrantScope scope);

    // 当前是否有待授权的交互
    bool hasPendingAuthorization() const;
    PermissionRequest pendingPermissionRequest() const;

    // -----------------------------------------------------------------------
    // 直接执行一个已知规则（供设置页「测试命令」按钮使用）
    //   参数直接给出，跳过解析；权限流程仍然完整走一遍。
    // -----------------------------------------------------------------------
    InteractionOutcome executeRule(const QString &ruleId,
                                  const QMap<QString, QString> &parameters);

signals:
    // 需要 UI 弹出授权对话框
    void authorizationRequired(const dc::PermissionRequest &request);

    // 交互结束（UI 更新气泡与历史）
    void interactionFinished(const dc::InteractionOutcome &outcome);

    // 中间状态变化（气泡显示「正在执行…」）
    void interactionProgress(const QString &taskId, const QString &message);

private:
    // 根据意图构造 PermissionRequest
    PermissionRequest buildPermissionRequest(const CommandCandidate &candidate) const;

    // 根据意图构造 TaskDefinition（含执行体）
    TaskDefinition buildTaskDefinition(const CommandCandidate &candidate) const;

    // 生成面向用户的结果文案
    QString composeDisplayMessage(const CommandCandidate &candidate) const;

    // 写交互历史（SRS 4.10 conversation / task_history）
    void recordInteraction(const InteractionRequest &request,
                          const InteractionOutcome &outcome);

    // 权限链路的公共前置部分
    InteractionOutcome beginExecution(const InteractionRequest &request,
                                     const CommandCandidate &candidate);

    // 未找到可执行命令时的提示
    InteractionOutcome buildNoMatchOutcome(const ParseResult &parse,
                                          const InteractionRequest &request) const;

    CommandRepository *m_repository = nullptr;
    CommandEngine *m_engine = nullptr;
    PermissionManager *m_permissions = nullptr;
    TaskScheduler *m_scheduler = nullptr;

    SystemService *m_system = nullptr;
    SearchService *m_search = nullptr;
    TranslationService *m_translation = nullptr;
    RecorderService *m_recorder = nullptr;
    SpeechService *m_speech = nullptr;
    HistoryService *m_history = nullptr;

    // 待授权状态
    bool m_hasPending = false;
    PermissionRequest m_pendingRequest;
    CommandCandidate m_pendingCandidate;
    InteractionRequest m_pendingInteraction;
    QDateTime m_pendingStartedAt;

    // -----------------------------------------------------------------------
    // 已提交但尚未结束的任务
    //
    //  作用有两个：
    //    1. m_pendingTaskIds —— 过滤 TaskScheduler::taskFinished。
    //       调度器是全局的，设置页「测试命令」也会往里提交任务；
    //       只有本类提交的任务才应该变成 interactionFinished 推给界面。
    //    2. m_pendingInteractions —— 任务结束时需要知道「原始输入是什么」，
    //       才能把这条交互写进历史记录。TaskInfo 里只有规则与意图，
    //       没有用户原话，所以必须在这里留存。
    // -----------------------------------------------------------------------
    QSet<QString> m_pendingTaskIds;
    QHash<QString, InteractionRequest> m_pendingInteractions;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::InteractionRequest)
Q_DECLARE_METATYPE(dc::InteractionOutcome)
