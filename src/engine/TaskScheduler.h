// ============================================================================
//  TaskScheduler.h — 任务队列、取消、超时与状态反馈
//
//  需求依据：SRS V1.1
//    3.3 TaskScheduler：任务队列、取消、超时、状态反馈
//    7.1 网络等待：异步执行，不阻塞 UI
//    7.2 稳定性：
//        「对任务提供取消机制；可取消任务不得在取消后继续执行后续步骤。」
//
//  设计说明（为什么需要独立的调度器）：
//    命令执行涉及多种耗时操作（启动进程、网络请求、文件扫描）。
//    如果在 UI 线程直接执行，桌宠会卡住 —— 直接违反 SRS 7.1。
//    调度器提供：
//      1. 串行执行有副作用的任务（避免两个命令同时改同一文件）
//      2. 并发执行只读任务（网络查询可并行）
//      3. 统一取消与超时语义
//      4. 通过信号把状态变化送回 UI（气泡提示的依据）
// ============================================================================
#pragma once

#include "model/PermissionTypes.h"
#include "model/Types.h"

#include <QDateTime>
#include <QFuture>
#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QUuid>
#include <QVariantMap>

#include <functional>
#include <QHash>
#include <QStringList>
#include <QVector>

namespace dc {

// ---------------------------------------------------------------------------
// 任务状态快照（UI 通过它渲染气泡文字与进度）
// ---------------------------------------------------------------------------
struct TaskInfo {
    QString taskId;
    QString ruleId;
    QString intent;
    QString title;                 // 一句话描述，如「打开 Chrome」
    TaskStatus status = TaskStatus::Pending;
    int progressPercent = -1;      // -1 表示进度不可知
    QString statusMessage;         // 当前阶段说明
    QString errorMessage;          // 失败原因（面向用户）
    QDateTime createdAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    int timeoutMs = 0;

    bool isTerminal() const
    {
        return status == TaskStatus::Succeeded
            || status == TaskStatus::Failed
            || status == TaskStatus::Cancelled
            || status == TaskStatus::TimedOut
            || status == TaskStatus::Denied;
    }

    qint64 elapsedMs() const;
};

// ---------------------------------------------------------------------------
// 任务执行体
//
//  约定：
//    - execute() 在工作线程中运行，不得触碰 UI
//    - 通过 ProgressCallback 上报中间状态（内部通过信号转发到 UI 线程）
//    - 返回 Result 表达成功/失败
//    - 必须定期检查 isCancelled()，以便及时响应取消
// ---------------------------------------------------------------------------
using TaskProgressCallback = std::function<void(int percent, const QString &message)>;
using TaskCancelProbe = std::function<bool()>;

struct TaskDefinition {
    QString ruleId;
    QString intent;
    QString title;

    // 是否需要独占执行（有副作用的任务：文件操作、启动进程）
    // false 的任务可并发（网络查询）
    bool exclusive = true;

    // 超时毫秒数；<= 0 表示使用调度器默认值
    int timeoutMs = 0;

    // 关联的权限请求（用于执行后写入审计）
    //
    //  为什么 TaskScheduler 要持有它：任务执行完成后需要落一条审计记录
    //  （谁、在什么授权下、对什么目标、结果如何）。审计记录需要规则 id
    //  和目标指纹，这两样都在 PermissionRequest 里。
    //  调度器自己不去查权限，只是把请求带过去 —— 判定发生在提交任务之前。
    PermissionRequest permissionRequest;
    bool hasPermissionRequest = false;

    // 执行体
    std::function<Result(const TaskProgressCallback &, const TaskCancelProbe &)> execute;
};

// ---------------------------------------------------------------------------
// TaskScheduler
// ---------------------------------------------------------------------------
class TaskScheduler : public QObject
{
    Q_OBJECT

public:
    explicit TaskScheduler(QObject *parent = nullptr);
    ~TaskScheduler() override;

    // 默认超时（SRS 7.2：超时应有可理解的处理）
    void setDefaultTimeoutMs(int ms);
    int defaultTimeoutMs() const;

    // 同时允许的并发任务上限（只读任务）
    void setMaxConcurrentTasks(int count);
    int maxConcurrentTasks() const;

    // -----------------------------------------------------------------------
    // 提交任务。返回 taskId（用于后续取消/查询）。
    // 若调度器已停止，返回空字符串。
    // -----------------------------------------------------------------------
    QString submit(const TaskDefinition &definition);

    // 取消指定任务。若任务尚未开始，直接标记为取消；
    // 若正在执行，置取消标志由执行体自行退出（SRS 7.2）。
    bool cancel(const QString &taskId);

    // 取消全部任务（退出程序时调用）
    void cancelAll();

    // 等待全部任务结束（带超时），供退出流程使用
    bool waitForAll(int timeoutMs = 5000);

    // -----------------------------------------------------------------------
    // 查询
    // -----------------------------------------------------------------------
    TaskInfo taskInfo(const QString &taskId) const;
    QVector<TaskInfo> activeTasks() const;
    QVector<TaskInfo> recentTasks(int limit = 20) const;
    bool isIdle() const;

    // 停止接收新任务（程序退出中）
    void shutdown();
    bool isShutdown() const;

signals:
    // 任务被接受（进入队列）
    void taskQueued(const dc::TaskInfo &info);
    // 任务开始执行
    void taskStarted(const dc::TaskInfo &info);
    // 进度更新
    void taskProgress(const QString &taskId, int percent, const QString &message);
    // 任务状态变化（含终止状态）
    void taskStatusChanged(const dc::TaskInfo &info);
    // 任务结束（终态，便于 UI 弹气泡）
    void taskFinished(const dc::TaskInfo &info);

private:
    struct InternalTask {
        TaskDefinition definition;
        TaskInfo info;
        bool cancelRequested = false;
        // 超时定时器 id
        int timeoutTimerId = 0;
    };

    void processQueue();
    void startTask(const QString &taskId);
    void finishTask(const QString &taskId, TaskStatus status,
                    const QString &errorMessage, const QString &detail);
    void updateInfo(const QString &taskId);
    void onTaskTimeout(const QString &taskId);

    // 内部实现：在工作线程中执行
    void runTaskBody(const QString &taskId);

    QHash<QString, InternalTask> m_tasks;
    QQueue<QString> m_pendingQueue;
    QStringList m_recentOrder;      // 最近完成任务顺序（用于 recentTasks）

    // 保护 m_tasks 中 cancelRequested 标志的读取。
    // 工作线程只通过锁读取这一个字段，主线程负责写；
    // 其它字段一律只在主线程访问，因此不需要全量加锁。
    mutable QMutex m_taskMutex;

    int m_defaultTimeoutMs = 30000;
    int m_maxConcurrent = 1;
    int m_runningExclusive = 0;
    int m_runningShared = 0;
    bool m_shutdown = false;

    static constexpr int kMaxRecentTasks = 100;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::TaskInfo)
