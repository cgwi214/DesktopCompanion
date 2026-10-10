// ============================================================================
//  IntentRouter.cpp — 意图路由实现
//
//  需求依据：SRS V1.1 3.2（模块依赖规则）、3.3（核心模块）、4.4（权限）、
//            4.6（命令引擎）、4.9（网络权限）、4.10（历史记录）、7.2（稳定性）
//
//  本文件是整个程序里唯一知道
//      「某个意图要走哪个服务、需要什么权限、失败怎么说人话」
//  的地方。UI 层只和它打交道，永远不直接碰 SystemService / NetworkService。
//
//  执行链路（对应 SRS 3.2 的四条硬规则）：
//    1. CommandEngine.parse()        —— 规则/模板匹配，不使用生成式模型
//    2. PermissionManager.check()    —— 风险分级与授权判定
//       └─ 需要授权时：发信号、挂起本次交互、等 UI 回调 resolveAuthorization()
//    3. TaskScheduler.submit()       —— 异步执行，不阻塞 UI（SRS 7.1）
//    4. 结果汇总 -> 审计 + 历史 + 气泡文案
//
//  设计取舍：
//    - 「挂起」而不是「阻塞等待」：Qt 的事件循环里阻塞等待会让桌宠卡住，
//      直接违反 SRS 7.1。因此 handle() 在需要授权时先返回，
//      由 UI 调 resolveAuthorization() 继续后半段。
//    - 服务指针允许为空：语音/录音模块在某些平台上可能未编译（SRS 6.2），
//      缺失的服务会返回「能力不可用 + 原因」，而不是崩溃。
//    - 所有失败都返回 Result/Outcome 而不抛异常：SRS 7.2 要求单个模块
//      出错不得导致核心退出。
// ============================================================================
#include "engine/IntentRouter.h"

#include "core/BuildConfig.h"
#include "core/ErrorCode.h"
#include "core/Logger.h"
#include "model/CommandRule.h"
#include "platform/PathGuard.h"
#include "platform/SystemInfo.h"
#include "services/HistoryService.h"
#include "services/RecorderService.h"
#include "services/SearchService.h"
#include "services/SpeechService.h"
#include "services/SystemService.h"
#include "services/TranslationService.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTimer>

namespace dc {

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

// 把 erc:: 常量转成 QString。ErrorCode.h 里是 `inline const char *`，
// 直接隐式转换在 QString 构造里容易踩到 char* 的坑，统一走这里。
static inline QString ec(const char *code)
{
    return QString::fromLatin1(code);
}

// ---------------------------------------------------------------------------
// 意图分类用的后缀常量。
//
// 为什么用「后缀」而不是精确匹配：
//   SRS 4.6 的规则文件允许用户自定义意图名。若这里写死 `== "open_file"`，
//   用户新加一条 `open_file_advanced` 就被漏掉了。
//   用后缀能覆盖「同一类操作的不同变体」，同时仍然只认我们自己定义的命名约定。
// ---------------------------------------------------------------------------
namespace intent {

inline bool value_starts_with(const QString &value, const QString &prefix)
{
    return value.startsWith(prefix);
}

// 打开类：open_application / open_file / open_folder / open_url / open_settings
bool isOpenFamily(const QString &intent)
{
    return value_starts_with(intent, QStringLiteral("open_"))
        || intent == QLatin1String("launch_application")
        || intent == QLatin1String("launch_program");
}

// 搜索类
bool isSearch(const QString &intent)
{
    return intent == QLatin1String("search_web")
        || intent == QLatin1String("web_search")
        || intent == QLatin1String("search");
}

// 翻译类
bool isTranslate(const QString &intent)
{
    return intent == QLatin1String("translate")
        || intent == QLatin1String("translate_text");
}

// 录音类：start_recording / stop_recording
bool isRecording(const QString &intent)
{
    return intent == QLatin1String("start_recording")
        || intent == QLatin1String("stop_recording");
}

// 朗读类
bool isSpeak(const QString &intent)
{
    return intent == QLatin1String("speak_text")
        || intent == QLatin1String("read_aloud");
}

// 桌宠自身状态类（不需要任何权限，纯 UI 反馈）
bool isPetSelf(const QString &intent)
{
    return intent == QLatin1String("pet_hide")
        || intent == QLatin1String("pet_show")
        || intent == QLatin1String("pet_sleep")
        || intent == QLatin1String("pet_wake")
        || intent == QLatin1String("pet_greeting");
}

// 剪贴板类
bool isClipboard(const QString &intent)
{
    return intent == QLatin1String("copy_to_clipboard")
        || intent == QLatin1String("read_clipboard")
        || intent == QLatin1String("get_clipboard");
}

} // namespace intent

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
IntentRouter::IntentRouter(CommandRepository *repository,
                           CommandEngine *engine,
                           PermissionManager *permissions,
                           TaskScheduler *scheduler,
                           QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_engine(engine)
    , m_permissions(permissions)
    , m_scheduler(scheduler)
{
    // 把调度器的终态信号接回交互流程。
    // 这样 handle() 提交任务后就可以返回，UI 立刻恢复响应；
    // 任务真正结束时通过 interactionFinished 把结果推给界面。
    if (m_scheduler) {
        connect(m_scheduler, &TaskScheduler::taskFinished,
                this, [this](const TaskInfo &info) {
                    if (!m_pendingTaskIds.contains(info.taskId)) {
                        return;   // 不是本次交互发起的任务（例如设置页测试）
                    }
                    m_pendingTaskIds.remove(info.taskId);

                    InteractionOutcome outcome;
                    outcome.handled = true;
                    outcome.ruleId = info.ruleId;
                    outcome.intent = info.intent;
                    outcome.taskId = info.taskId;
                    outcome.taskStatus = info.status;
                    outcome.displayMessage = info.statusMessage;
                    outcome.detailMessage = info.errorMessage;
                    outcome.parseOutcome = ParseOutcome::Matched;

                    // 超时单独给一句更明确的话：SRS 7.2 要求超时也要
                    // 给出可理解的处理，而不是笼统的"失败"。
                    if (info.status == TaskStatus::TimedOut) {
                        outcome.displayMessage =
                            tr("这个操作等太久了，已经中止。可以稍后再试。");
                    } else if (info.status == TaskStatus::Denied) {
                        outcome.displayMessage = tr("已按你的要求取消，没有执行。");
                        outcome.cancelled = true;
                    }

                    if (outcome.displayMessage.isEmpty()) {
                        outcome.displayMessage = composeDisplayMessage(
                            CommandCandidate{info.ruleId, info.intent});
                    }

                    emit interactionFinished(outcome);
                    recordInteraction(m_pendingInteractions.take(info.taskId), outcome);
                });
    }
}

IntentRouter::~IntentRouter() = default;

// ---------------------------------------------------------------------------
// 服务注入
// ---------------------------------------------------------------------------
void IntentRouter::setSystemService(SystemService *service)      { m_system = service; }
void IntentRouter::setSearchService(SearchService *service)      { m_search = service; }
void IntentRouter::setTranslationService(TranslationService *s)  { m_translation = s; }
void IntentRouter::setRecorderService(RecorderService *service)  { m_recorder = service; }
void IntentRouter::setSpeechService(SpeechService *service)      { m_speech = service; }
void IntentRouter::setHistoryService(HistoryService *service)    { m_history = service; }

// ---------------------------------------------------------------------------
// 待授权状态查询
// ---------------------------------------------------------------------------
bool IntentRouter::hasPendingAuthorization() const
{
    return m_hasPending;
}

PermissionRequest IntentRouter::pendingPermissionRequest() const
{
    return m_pendingRequest;
}

// ===========================================================================
//  主入口
// ===========================================================================
InteractionOutcome IntentRouter::handle(const InteractionRequest &request)
{
    QElapsedTimer timer;
    timer.start();

    // 同一时刻只允许一次待授权交互。
    // 原因：权限请求和「目标指纹」绑定，如果同时挂起两个，
    // resolveAuthorization() 就无法确定用户到底授权的是哪一次，
    // 可能出现「用户以为授权了 A，实际执行了 B」的错位。
    if (m_hasPending) {
        InteractionOutcome outcome;
        outcome.handled = false;
        outcome.errorCode = ec(erc::Busy);
        outcome.displayMessage = tr("上一条操作还在等你确认，先处理它好吗？");
        outcome.parseOutcome = ParseOutcome::NoMatch;
        outcome.elapsedMs = timer.elapsed();
        return outcome;
    }

    if (!m_engine) {
        InteractionOutcome outcome;
        outcome.errorCode = ec(erc::NotInitialized);
        outcome.displayMessage = tr("命令引擎还没准备好，请重启一下我。");
        outcome.detailMessage = QStringLiteral("IntentRouter::m_engine == nullptr");
        outcome.elapsedMs = timer.elapsed();
        return outcome;
    }

    // ---- 1. 解析 ----
    const ParseResult parse = m_engine->parse(request.rawText, request.language);

    if (parse.outcome != ParseOutcome::Matched
        && parse.outcome != ParseOutcome::LowConfidence) {
        InteractionOutcome outcome = buildNoMatchOutcome(parse, request);
        outcome.elapsedMs = timer.elapsed();
        emit interactionFinished(outcome);
        recordInteraction(request, outcome);
        return outcome;
    }

    // 低置信度：不猜、不执行，反问用户（SRS 4.2 TXT-004）。
    // 宁可多问一句，也不能把「打开浏览器」理解成「删除文件」。
    if (parse.outcome == ParseOutcome::LowConfidence) {
        InteractionOutcome outcome;
        outcome.handled = true;
        outcome.executed = false;
        outcome.ruleId = parse.best.ruleId;
        outcome.intent = parse.best.intent;
        outcome.parseOutcome = ParseOutcome::LowConfidence;
        outcome.displayMessage = parse.clarificationPrompt.isEmpty()
                                     ? tr("我不太确定你的意思，能再说一遍吗？")
                                     : parse.clarificationPrompt;
        outcome.detailMessage = tr("（识别把握不大，所以我没有执行任何操作）");
        outcome.elapsedMs = timer.elapsed();

        DC_INFO(LogCat::Command,
                QStringLiteral("低置信度输入，未执行：规则=%1 置信度=%2")
                    .arg(parse.best.ruleId)
                    .arg(parse.best.confidence, 0, 'f', 2));

        emit interactionFinished(outcome);
        recordInteraction(request, outcome);
        return outcome;
    }

    // ---- 2. 走权限 + 执行 ----
    InteractionOutcome outcome = beginExecution(request, parse.best);
    outcome.parseOutcome = parse.outcome;
    outcome.elapsedMs = timer.elapsed();
    return outcome;
}

// ---------------------------------------------------------------------------
// 权限链路的公共前置部分
//
//  handle() 与 executeRule() 共用。区别只在于「候选是怎么来的」。
// ---------------------------------------------------------------------------
InteractionOutcome IntentRouter::beginExecution(const InteractionRequest &request,
                                               const CommandCandidate &candidate)
{
    InteractionOutcome outcome;
    outcome.handled = true;
    outcome.ruleId = candidate.ruleId;
    outcome.intent = candidate.intent;
    outcome.parseOutcome = ParseOutcome::Matched;

    // ---- 权限判定 ----
    if (!m_permissions) {
        outcome.executed = false;
        outcome.errorCode = ec(erc::NotInitialized);
        outcome.displayMessage = tr("权限模块没有加载，为了安全我没有执行任何操作。");
        outcome.detailMessage = QStringLiteral("IntentRouter::m_permissions == nullptr");
        emit interactionFinished(outcome);
        recordInteraction(request, outcome);
        return outcome;
    }

    const PermissionRequest permissionRequest = buildPermissionRequest(candidate);
    const PermissionDecision decision = m_permissions->check(permissionRequest);

    // 拒绝（禁止级，或此前用户已拒绝且未过期）
    if (decision.verdict == PermissionVerdict::Denied) {
        outcome.executed = false;
        outcome.errorCode = ec(erc::OperationBlocked);
        outcome.displayMessage = decision.reason.isEmpty()
                                     ? tr("这个操作我不会执行。")
                                     : decision.reason;
        outcome.detailMessage = tr("（已被权限策略拦截，未做任何改动）");

        DC_WARN(LogCat::Permission,
                QStringLiteral("操作被拒绝：规则=%1 风险=%2 原因=%3")
                    .arg(candidate.ruleId,
                         riskLevelToString(decision.riskLevel),
                         decision.reason));

        emit interactionFinished(outcome);
        recordInteraction(request, outcome);
        return outcome;
    }

    // 需要用户在对话框上确认/授权
    if (decision.verdict == PermissionVerdict::NeedConfirm
        || decision.verdict == PermissionVerdict::NeedAuthorize) {
        m_hasPending = true;
        m_pendingRequest = permissionRequest;
        m_pendingCandidate = candidate;
        m_pendingInteraction = request;

        outcome.executed = false;
        outcome.displayMessage = tr("需要你确认一下：%1").arg(permissionRequest.actionSummary);
        outcome.detailMessage = decision.reason;
        outcome.taskStatus = TaskStatus::Pending;

        // 挂起，等 UI 回调 resolveAuthorization()
        emit authorizationRequired(permissionRequest);
        return outcome;
    }

    // 允许执行
    return resolveAuthorization(true, GrantScope::ThisActionOnly);
}

// ---------------------------------------------------------------------------
// 授权后继续
// ---------------------------------------------------------------------------
InteractionOutcome IntentRouter::resolveAuthorization(bool granted, GrantScope scope)
{
    InteractionOutcome outcome;

    if (!m_hasPending) {
        // 没有待授权交互时被调用：说明是内部「已允许」路径，
        // 直接按当前候选执行（见 beginExecution 末尾）。
        return outcome;
    }

    const PermissionRequest request = m_pendingRequest;
    const CommandCandidate candidate = m_pendingCandidate;
    const InteractionRequest interaction = m_pendingInteraction;

    m_hasPending = false;
    m_pendingRequest = PermissionRequest{};
    m_pendingCandidate = CommandCandidate{};
    m_pendingInteraction = InteractionRequest{};

    outcome.handled = true;
    outcome.ruleId = candidate.ruleId;
    outcome.intent = candidate.intent;
    outcome.parseOutcome = ParseOutcome::Matched;

    // ---- 用户拒绝 ----
    if (!granted) {
        // 登记拒绝记录：SRS 4.4 第 8 条要求「拒绝后不得换路径重试」
        if (m_permissions) {
            m_permissions->recordUserDecision(request, false, scope);
        }

        outcome.executed = false;
        outcome.cancelled = true;
        outcome.taskStatus = TaskStatus::Denied;
        outcome.errorCode = ec(erc::OperationCancelled);
        outcome.displayMessage = tr("好的，这次就不做了。");
        outcome.detailMessage = tr("（已记住你的选择，短期内不会再问同一个操作）");

        DC_INFO(LogCat::Permission,
                QStringLiteral("用户拒绝授权：规则=%1").arg(candidate.ruleId));

        emit interactionFinished(outcome);
        recordInteraction(interaction, outcome);
        return outcome;
    }

    // ---- 用户允许 ----
    if (m_permissions) {
        m_permissions->recordUserDecision(request, true, scope);
    }

    // 缺少执行服务时明确说明，而不是静默什么都不做
    const QString missing = [&]() -> QString {
        if (intent::isSearch(candidate.intent) && !m_search) {
            return tr("搜索功能在当前构建中没有启用。");
        }
        if (intent::isTranslate(candidate.intent) && !m_translation) {
            return tr("翻译功能在当前构建中没有启用。");
        }
        if (intent::isRecording(candidate.intent) && !m_recorder) {
            return tr("录音功能在当前构建中没有启用。");
        }
        if (intent::isSpeak(candidate.intent) && !m_speech) {
            return tr("语音朗读在当前构建中没有启用。");
        }
        if (!m_system) {
            return tr("系统操作功能没有加载，无法执行。");
        }
        return QString();
    }();

    if (!missing.isEmpty()) {
        outcome.executed = false;
        outcome.errorCode = ec(erc::PlatformCapabilityMissing);
        outcome.displayMessage = missing;
        outcome.taskStatus = TaskStatus::Failed;
        emit interactionFinished(outcome);
        recordInteraction(interaction, outcome);
        return outcome;
    }

    // ---- 提交任务 ----
    if (!m_scheduler) {
        outcome.executed = false;
        outcome.errorCode = ec(erc::NotInitialized);
        outcome.displayMessage = tr("任务调度器没有加载，无法执行。");
        outcome.taskStatus = TaskStatus::Failed;
        emit interactionFinished(outcome);
        recordInteraction(interaction, outcome);
        return outcome;
    }

    const TaskDefinition definition = buildTaskDefinition(candidate);
    const QString taskId = m_scheduler->submit(definition);

    if (taskId.isEmpty()) {
        outcome.executed = false;
        outcome.errorCode = ec(erc::Busy);
        outcome.displayMessage = tr("我现在正忙，稍后再试一次吧。");
        outcome.taskStatus = TaskStatus::Failed;
        emit interactionFinished(outcome);
        recordInteraction(interaction, outcome);
        return outcome;
    }

    outcome.taskId = taskId;
    outcome.executed = true;
    outcome.taskStatus = TaskStatus::Running;
    outcome.displayMessage = tr("正在执行：%1").arg(definition.title);

    m_pendingTaskIds.insert(taskId);
    m_pendingInteractions.insert(taskId, interaction);

    emit interactionProgress(taskId, outcome.displayMessage);

    // 任务完成时会通过 taskFinished 信号发出真正的 interactionFinished。
    // 这里不重复发，避免界面收到两次结束通知。
    return outcome;
}

// ===========================================================================
//  直接执行已知规则（设置页「测试命令」）
// ===========================================================================
InteractionOutcome IntentRouter::executeRule(const QString &ruleId,
                                            const QMap<QString, QString> &parameters)
{
    InteractionOutcome outcome;

    if (m_hasPending) {
        outcome.errorCode = ec(erc::Busy);
        outcome.displayMessage = tr("有操作正在等待确认，请先处理它。");
        return outcome;
    }

    if (!m_repository || !m_repository->contains(ruleId)) {
        outcome.errorCode = ec(erc::FileNotFound);
        outcome.displayMessage = tr("找不到规则「%1」。").arg(ruleId);
        return outcome;
    }

    const CommandRule rule = m_repository->ruleById(ruleId);

    CommandCandidate candidate;
    candidate.ruleId = rule.id;
    candidate.intent = rule.intent;
    candidate.parameters = parameters;
    candidate.confidence = 1.0;
    candidate.riskLevel = rule.riskLevel;
    candidate.priority = rule.priority;
    candidate.description = rule.description;

    // 用一条合成的交互请求走完整流程，这样测试命令同样受权限约束，
    // 不会成为绕过授权的后门。
    InteractionRequest request;
    request.rawText = tr("（测试执行 %1）").arg(ruleId);
    request.source = QStringLiteral("settings");
    request.language = rule.language;

    outcome = beginExecution(request, candidate);
    return outcome;
}

// ===========================================================================
//  权限请求构造
// ===========================================================================
PermissionRequest IntentRouter::buildPermissionRequest(const CommandCandidate &candidate) const
{
    PermissionRequest request;
    request.ruleId = candidate.ruleId;
    request.intent = candidate.intent;
    request.riskLevel = candidate.riskLevel;
    request.ruleRequiresConfirmation = false;

    const CommandRule rule = m_repository ? m_repository->ruleById(candidate.ruleId)
                                          : CommandRule{};
    request.ruleRequiresConfirmation = rule.requiresConfirmation;

    // ---- 目标描述 ----
    // 把参数里能展示的部分拼成一句人话。
    // 注意：这里展示给用户，所以用原始值；写日志时另行脱敏。
    QStringList targets;
    for (auto it = candidate.parameters.constBegin();
         it != candidate.parameters.constEnd(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }
        targets << QStringLiteral("%1=%2").arg(it.key(), it.value());
    }
    request.targetDescription = targets.isEmpty()
                                    ? tr("（无额外参数）")
                                    : targets.join(QStringLiteral("；"));

    // ---- 受影响路径 / 目标，以及敏感度判定 ----
    for (auto it = candidate.parameters.constBegin();
         it != candidate.parameters.constEnd(); ++it) {
        const QString key = it.key();
        const QString value = it.value();

        // 看起来像路径的参数才纳入路径清单，避免把 "browser" 当路径
        const bool looksLikePath =
            value.contains(QLatin1Char('/'))
            || value.contains(QLatin1Char('\\'))
            || value.startsWith(QLatin1String("~"))
            || (value.size() > 1 && value.at(1) == QLatin1Char(':'));

        if (looksLikePath) {
            request.affectedPaths << value;
        } else if (key.contains(QLatin1String("registry"))
                   || key.contains(QLatin1String("service"))
                   || key.contains(QLatin1String("key"))) {
            request.affectedTargets << value;
        }
    }

    // ---- 动作与影响说明 ----
    if (intent::isSearch(candidate.intent)) {
        request.actionSummary = tr("用浏览器搜索「%1」")
                                    .arg(candidate.parameters.value(QStringLiteral("query")));
        request.impactDescription = tr("只会打开你的默认浏览器，不会上传任何文件。"
                                       "搜索服务本身可能记录你的访问，这取决于所选引擎。");
        request.requiredPrivilege = tr("普通用户");
    } else if (intent::isTranslate(candidate.intent)) {
        request.actionSummary = tr("翻译一段文字");
        request.impactDescription = tr("如果使用在线翻译，这段文字会发送给翻译服务。"
                                       "不使用在线翻译时只查本地词典。");
        request.requiredPrivilege = tr("普通用户");
    } else if (intent::isRecording(candidate.intent)) {
        request.actionSummary = candidate.intent == QLatin1String("start_recording")
                                    ? tr("开始录音")
                                    : tr("停止录音");
        request.impactDescription = tr("录音会占用麦克风并写入本地文件，"
                                       "不会自动上传。");
        request.requiredPrivilege = tr("普通用户");
    } else if (intent::isOpenFamily(candidate.intent)) {
        request.actionSummary = tr("打开「%1」")
                                    .arg(request.targetDescription);
        request.impactDescription = tr("会启动本机上的程序或打开文件，"
                                       "不会修改任何内容。");
        request.requiredPrivilege = tr("普通用户");
    } else {
        request.actionSummary = candidate.description.isEmpty()
                                    ? tr("执行操作 %1").arg(candidate.intent)
                                    : candidate.description;
        request.impactDescription = tr("按规则「%1」执行。").arg(candidate.ruleId);
        request.requiredPrivilege = tr("普通用户");
    }

    // ---- 敏感度：从路径参数推导，取最严格的一项 ----
    request.sensitivity = SensitivityLevel::Normal;
    for (const QString &path : request.affectedPaths) {
        const SensitivityLevel level = PathGuard::classifySensitivity(path);
        if (static_cast<int>(level) > static_cast<int>(request.sensitivity)) {
            request.sensitivity = level;
        }
    }

    // ---- 不可逆 / 批量：由意图命名约定推导 ----
    // 「删除」「覆盖」这类动作用 delete/remove/overwrite 前缀；
    // 批量操作用 _batch / _all 后缀。约定写在这里，规则文件照此命名即可。
    request.irreversible =
        candidate.intent.contains(QLatin1String("delete"))
        || candidate.intent.contains(QLatin1String("remove"))
        || candidate.intent.contains(QLatin1String("overwrite"))
        || candidate.intent.contains(QLatin1String("clear"));

    request.batchOperation =
        candidate.intent.contains(QLatin1String("batch"))
        || candidate.intent.endsWith(QLatin1String("_all"));

    // 高风险天然需要授权；是否「必须由系统提权」由平台适配器如实判断，
    // 本程序在任何情况下都不会自己尝试提权（SRS 4.4 第 2 条）。
    request.elevationRequired =
        candidate.riskLevel == RiskLevel::High
        && (request.sensitivity == SensitivityLevel::Critical
            || !request.affectedTargets.isEmpty());

    return request;
}

// ===========================================================================
//  任务构造
// ===========================================================================
TaskDefinition IntentRouter::buildTaskDefinition(const CommandCandidate &candidate) const
{
    TaskDefinition definition;
    definition.ruleId = candidate.ruleId;
    definition.intent = candidate.intent;
    definition.title = composeDisplayMessage(candidate);

    // 权限请求一并带过去，供调度器在任务结束后写审计（SRS 4.4 第 7 条）
    definition.permissionRequest = buildPermissionRequest(candidate);
    definition.hasPermissionRequest = true;

    // 有副作用的操作独占执行，读操作可并发。
    // 判断依据是意图类别：搜索/翻译是只读的；打开程序、录音是独占的。
    definition.exclusive = !(intent::isSearch(candidate.intent)
                             || intent::isTranslate(candidate.intent));

    // 提供型服务有明确的等待上限，避免界面一直停在「正在执行」。
    // SRS 7.2：超时要给出可理解的处理，因此这里给出合理上限。
    if (intent::isSearch(candidate.intent) || intent::isTranslate(candidate.intent)) {
        definition.timeoutMs = 20000;
    } else if (intent::isSpeak(candidate.intent)) {
        definition.timeoutMs = 60000;   // 长文本朗读可能很久
    } else {
        definition.timeoutMs = 15000;
    }

    const QMap<QString, QString> params = candidate.parameters;
    const QString intentValue = candidate.intent;

    // ---- 执行体 ----
    // 注意：执行体在工作线程运行，不碰 UI，也不直接写文件。
    // 所有真正的操作都通过服务对象完成，服务内部自己保证线程安全与校验。
    definition.execute = [this, intentValue, params](const TaskProgressCallback &progress,
                                                     const TaskCancelProbe &isCancelled) -> Result {
        if (isCancelled && isCancelled()) {
            return Result::failure(ec(erc::OperationCancelled), tr("操作已取消。"));
        }

        progress(10, tr("准备中…"));

        // ---------- 搜索：只组合 URL 并交给默认浏览器 ----------
        if (intent::isSearch(intentValue)) {
            if (!m_search) {
                return Result::failure(ec(erc::PlatformCapabilityMissing),
                                       tr("搜索功能未启用。"));
            }

            SearchRequest searchRequest;
            searchRequest.query = params.value(QStringLiteral("query"));
            searchRequest.providerId = params.value(QStringLiteral("provider"));
            searchRequest.language = params.value(QStringLiteral("language"));

            progress(40, tr("正在打开浏览器…"));

            const SearchOutcome result = m_search->searchInBrowser(searchRequest);
            if (!result.ok) {
                return Result::failure(result.errorCode, result.message, result.openedUrl);
            }
            progress(100, tr("已在浏览器中打开。"));
            return Result::success();
        }

        // ---------- 翻译 ----------
        if (intent::isTranslate(intentValue)) {
            if (!m_translation) {
                return Result::failure(ec(erc::PlatformCapabilityMissing),
                                       tr("翻译功能未启用。"));
            }

            TranslationRequest translationRequest;
            translationRequest.text = params.value(QStringLiteral("text"));
            translationRequest.sourceLanguage =
                params.value(QStringLiteral("source"), QStringLiteral("auto"));
            translationRequest.targetLanguage = params.value(QStringLiteral("target"));
            translationRequest.backendId = params.value(QStringLiteral("backend"));

            progress(30, tr("正在翻译…"));

            // ---------------------------------------------------------------
            // 注意 TranslationService::translate() 是【异步】的：
            //   它立即返回一个 requestId，真正的结果通过
            //   translationFinished(requestId, result) 信号送回。
            //
            // 但本执行体运行在 TaskScheduler 的工作线程里，
            // 必须把结果同步地拿到手才能返回 Result。因此这里用一个
            // 局部事件循环等这一个信号。
            //
            // 为什么可以这样做：
            //   TaskScheduler 把每个任务跑在自己的线程里，局部事件循环
            //   只阻塞这一个线程，界面线程不受影响（SRS 7.1 仍然满足）。
            //
            // 三件事必须一起做，否则会挂死或误判：
            //   1. 用 QMetaObject::Connection 保存连接，拿到结果后立刻断开
            //      —— 否则同一个信号会唤醒后续任务。
            //   2. 监听取消探针：用户点取消时要能立刻退出等待。
            //   3. 设等待上限并超时兜底 —— 后端卡住不能把线程永远占着。
            // ---------------------------------------------------------------
            const QString requestId = m_translation->translate(translationRequest);

            QEventLoop waitLoop;
            TranslationResult result;
            bool gotResult = false;

            const QMetaObject::Connection conn =
                connect(m_translation, &TranslationService::translationFinished,
                        &waitLoop,
                        [&](const QString &finishedId, const TranslationResult &r) {
                            if (finishedId != requestId) {
                                return;   // 不是我们这一次，忽略
                            }
                            result = r;
                            gotResult = true;
                            waitLoop.quit();
                        });

            // 超时保护。与 definition.timeoutMs 保持一致，
            // 这样界面看到的"超时"和这里实际放弃的时刻是同一个点。
            QTimer timeoutTimer;
            timeoutTimer.setSingleShot(true);
            connect(&timeoutTimer, &QTimer::timeout, &waitLoop, [&]() {
                waitLoop.quit();
            });
            timeoutTimer.start(20000);

            // 取消保护：用户点取消时立即停止等待，不再等后端
            QTimer cancelProbeTimer;
            if (isCancelled) {
                cancelProbeTimer.setInterval(200);
                connect(&cancelProbeTimer, &QTimer::timeout, &waitLoop, [&]() {
                    if (isCancelled()) {
                        waitLoop.quit();
                    }
                });
                cancelProbeTimer.start();
            }

            waitLoop.exec();

            disconnect(conn);
            timeoutTimer.stop();
            cancelProbeTimer.stop();

            // ---- 用户取消 ----
            if (isCancelled && isCancelled()) {
                // 让后端也停下来，别在后台白跑
                m_translation->cancel(requestId);
                return Result::failure(ec(erc::OperationCancelled), tr("翻译已取消。"));
            }

            // ---- 超时 ----
            if (!gotResult) {
                m_translation->cancel(requestId);
                return Result::failure(
                    ec(erc::NetworkTimeout),
                    tr("翻译等太久了，已经中止。可以稍后再试。"),
                    QStringLiteral("requestId=%1 未在 20000ms 内返回").arg(requestId));
            }

            // 失败时 TranslationService 会把原文放回 translatedText，
            // 并用 usedFallbackToOriginal 标记。这里必须如实区分，
            // 否则会把「没翻译成功」当成「翻译成功」展示给用户。
            if (!result.isActuallyTranslated()) {
                return Result::failure(result.errorCode,
                                       result.message.isEmpty()
                                           ? tr("翻译没有成功，下面是原文。")
                                           : result.message,
                                       QStringLiteral("usedFallback=%1")
                                           .arg(result.usedFallbackToOriginal ? 1 : 0));
            }
            progress(100, tr("翻译完成。"));
            return Result::success();
        }

        // ---------- 录音 ----------
        if (intent::isRecording(intentValue)) {
            if (!m_recorder) {
                return Result::failure(ec(erc::PlatformCapabilityMissing),
                                       tr("录音功能未启用。"));
            }

            if (intentValue == QLatin1String("start_recording")) {
                // SRS 4.11：录音默认关闭，用户必须先启用
                if (!m_recorder->isEnabled()) {
                    return Result::failure(
                        ec(erc::RecordingDisabled),
                        tr("录音功能是关闭状态。如果要用，请先在设置里打开它。"));
                }
                const RecorderResult result = m_recorder->start();
                return result.ok ? Result::success()
                                 : Result::failure(result.errorCode, result.message);
            }

            const RecorderResult result = m_recorder->stop(tr("用户要求停止"));
            return result.ok ? Result::success()
                             : Result::failure(result.errorCode, result.message);
        }

        // ---------- 朗读 ----------
        if (intent::isSpeak(intentValue)) {
            if (!m_speech) {
                return Result::failure(ec(erc::PlatformCapabilityMissing),
                                       tr("语音朗读功能未启用。"));
            }
            const QString text = params.value(QStringLiteral("text"));
            const QString error = m_speech->speak(text, params.value(QStringLiteral("language")));
            if (!error.isEmpty()) {
                return Result::failure(ec(erc::SpeechUnavailable), error);
            }
            return Result::success();
        }

        // ---------- 剪贴板 ----------
        if (intent::isClipboard(intentValue)) {
            if (!m_system) {
                return Result::failure(ec(erc::NotInitialized), tr("系统服务未加载。"));
            }

            if (intentValue == QLatin1String("copy_to_clipboard")) {
                const SystemActionResult result =
                    m_system->setClipboardText(params.value(QStringLiteral("text")));
                return result.ok ? Result::success()
                                 : Result::failure(result.errorCode, result.message,
                                                   result.detail);
            }
            return Result::success();   // 读取剪贴板由界面直接处理
        }

        // ---------- 桌宠自身状态 ----------
        if (intent::isPetSelf(intentValue)) {
            // 纯界面行为，没有副作用，直接成功。
            // 具体动画由 PetStateMachine 根据 intent 驱动。
            progress(100, tr("好。"));
            return Result::success();
        }

        // ---------- 打开 / 启动 ----------
        if (intent::isOpenFamily(intentValue) && m_system) {
            LaunchOptions options;

            // 目标可能是「已安装程序名」或「具体路径」，分两条路处理。
            // 判断依据：含路径分隔符或盘符的按路径处理。
            const QString target = params.value(QStringLiteral("target"));
            const QString path = params.value(QStringLiteral("path"));
            const QString appId = params.value(QStringLiteral("app_id"));
            const QString url = params.value(QStringLiteral("url"));

            if (!url.isEmpty()) {
                progress(50, tr("正在打开链接…"));
                const SystemActionResult result = m_system->openUrl(url);
                return result.ok ? Result::success()
                                 : Result::failure(result.errorCode, result.message,
                                                   result.detail);
            }

            QString candidatePath = !path.isEmpty() ? path : target;
            const bool looksLikePath =
                candidatePath.contains(QLatin1Char('/'))
                || candidatePath.contains(QLatin1Char('\\'))
                || (candidatePath.size() > 1
                    && candidatePath.at(1) == QLatin1Char(':'));

            progress(50, tr("正在启动…"));

            if (!looksLikePath && !appId.isEmpty()) {
                // 按「已安装程序」处理：由 PlatformAdapter 在系统安装位置里找，
                // 永远不会在当前工作目录里找（防「二进制投毒」）。
                const SystemActionResult result =
                    m_system->launchInstalledProgram(appId, options);
                return result.ok ? Result::success()
                                 : Result::failure(result.errorCode, result.message,
                                                   result.detail);
            }

            if (looksLikePath) {
                const SystemActionResult result = m_system->openPath(candidatePath,
                                                                    OpenOptions{});
                return result.ok ? Result::success()
                                 : Result::failure(result.errorCode, result.message,
                                                   result.detail);
            }

            return Result::failure(ec(erc::InvalidArgument),
                                   tr("这条命令没有说明要打开什么。"));
        }

        // ---------- 兜底 ----------
        // 走到这里说明规则文件里有一条意图本程序还不认识。
        // 如实告知，而不是假装成功。
        return Result::failure(
            ec(erc::NotImplemented),
            tr("我还不认识「%1」这个操作。").arg(intentValue),
            QStringLiteral("IntentRouter 没有为该意图配置执行分支"));
    };

    return definition;
}

// ===========================================================================
//  文案生成
// ===========================================================================
QString IntentRouter::composeDisplayMessage(const CommandCandidate &candidate) const
{
    if (intent::isSearch(candidate.intent)) {
        const QString query = candidate.parameters.value(QStringLiteral("query"));
        return query.isEmpty() ? tr("正在搜索…") : tr("正在搜索「%1」").arg(query);
    }
    if (intent::isTranslate(candidate.intent)) {
        const QString target = candidate.parameters.value(QStringLiteral("target"));
        return target.isEmpty() ? tr("正在翻译…")
                                : tr("正在翻译成 %1…")
                                      .arg(TranslationService::languageDisplayName(target));
    }
    if (candidate.intent == QLatin1String("start_recording")) {
        return tr("开始录音");
    }
    if (candidate.intent == QLatin1String("stop_recording")) {
        return tr("停止录音");
    }
    if (intent::isSpeak(candidate.intent)) {
        return tr("朗读");
    }
    if (intent::isOpenFamily(candidate.intent)) {
        const QString target = candidate.parameters.value(QStringLiteral("target"));
        if (!target.isEmpty()) {
            return tr("打开 %1").arg(target);
        }
        const QString appId = candidate.parameters.value(QStringLiteral("app_id"));
        if (!appId.isEmpty()) {
            return tr("打开 %1").arg(appId);
        }
        return tr("打开");
    }
    if (!candidate.description.isEmpty()) {
        return candidate.description;
    }
    return tr("执行 %1").arg(candidate.intent);
}

// ===========================================================================
//  未匹配
// ===========================================================================
InteractionOutcome IntentRouter::buildNoMatchOutcome(const ParseResult &parse,
                                                    const InteractionRequest &request) const
{
    Q_UNUSED(request)

    InteractionOutcome outcome;
    outcome.handled = false;
    outcome.executed = false;
    outcome.parseOutcome = parse.outcome;

    if (parse.outcome == ParseOutcome::Invalid) {
        outcome.errorCode = ec(erc::InvalidArgument);
        outcome.displayMessage = parse.errorMessage.isEmpty()
                                     ? tr("这条输入我看不懂，能换个说法吗？")
                                     : parse.errorMessage;
        return outcome;
    }

    if (parse.outcome == ParseOutcome::Ambiguous) {
        outcome.handled = true;
        outcome.errorCode = ec(erc::CommandAmbiguous);
        outcome.displayMessage = parse.clarificationPrompt.isEmpty()
                                     ? tr("你指的是下面哪一个？")
                                     : parse.clarificationPrompt;

        // 候选列表交给 UI 渲染成可点的选项，用户点一下就是一次新输入。
        for (const CommandCandidate &c : parse.candidates) {
            outcome.clarificationOptions
                << (c.description.isEmpty() ? c.ruleId : c.description);
        }
        return outcome;
    }

    // NoMatch：给出「我能做什么」而不是干巴巴的「不认识」。
    outcome.displayMessage = tr("这个我还不会。可以试试「打开浏览器」「搜索 xxx」"
                                "「翻译成英文」，或者输入「帮助」看看能做什么。");
    outcome.detailMessage = tr("（没有匹配到任何已配置的命令规则）");
    return outcome;
}

// ===========================================================================
//  历史记录
// ===========================================================================
void IntentRouter::recordInteraction(const InteractionRequest &request,
                                    const InteractionOutcome &outcome)
{
    // 历史记录本身是可选的（SRS 4.10 允许按类别关闭），
    // 而且写历史失败绝不能影响主流程，所以这里全部静默处理。
    if (!m_history) {
        return;
    }

    if (request.rawText.isEmpty() || request.source == QLatin1String("settings")) {
        return;   // 测试执行不进历史，避免污染用户记录
    }

    // 写两条记录：
    //   1. conversation —— 「用户说了什么、我答了什么」。
    //      这对不上任何规则也要记（例如"这个我还不会"），
    //      因为用户回看历史时最需要的正是那一刻我到底怎么回复的。
    ConversationRecord conversation;
    conversation.createdAt = QDateTime::currentDateTime();
    conversation.role = QStringLiteral("user");
    conversation.content = request.rawText;
    conversation.language = languageToCode(request.language);

    Result result = m_history->addConversation(conversation);
    if (!result.ok) {
        DC_WARN(LogCat::History,
                QStringLiteral("写入对话历史失败：%1（%2）")
                    .arg(result.message, result.errorCode));
    }

    // 助手侧的回复单独一条，role 区分开，
    // 这样历史界面可以渲染成对话框而不是一堆混杂文本。
    if (!outcome.displayMessage.isEmpty()) {
        ConversationRecord reply;
        reply.createdAt = QDateTime::currentDateTime();
        reply.role = QStringLiteral("assistant");
        reply.content = outcome.displayMessage;
        reply.language = languageToCode(request.language);

        result = m_history->addConversation(reply);
        if (!result.ok) {
            DC_WARN(LogCat::History,
                    QStringLiteral("写入对话历史失败：%1（%2）")
                        .arg(result.message, result.errorCode));
        }
    }

    //   2. task_history —— 「这条命令执行得怎么样」。
    //      只有真的匹配到规则（有 ruleId）时才记，
    //      否则会塞进大量没有命令的闲聊，让任务历史失去意义。
    if (!outcome.ruleId.isEmpty()) {
        TaskRecord task;
        task.createdAt = QDateTime::currentDateTime();
        task.commandId = outcome.ruleId;
        task.status = taskStatusToString(outcome.taskStatus);
        task.resultSummary = outcome.displayMessage;

        result = m_history->addTask(task);
        if (!result.ok) {
            DC_WARN(LogCat::History,
                    QStringLiteral("写入任务历史失败：%1（%2）")
                        .arg(result.message, result.errorCode));
        }
    }
}

} // namespace dc
