// ============================================================================
//  TaskScheduler.cpp — 任务调度实现
//
//  需求依据：SRS V1.1 3.3、7.1、7.2
//
//  线程模型说明：
//    本实现刻意保持简单 —— 使用 QThreadPool + QFutureWatcher 而不是
//    自建线程。原因：
//      1. 任务数量在桌宠场景下很少（用户一次说一句话）
//      2. QThreadPool 会自动复用线程，避免频繁创建销毁
//      3. 取消语义通过「协作式取消标志」实现 —— 执行体需主动检查，
//         这与 SRS 7.2「可取消任务不得在取消后继续执行后续步骤」一致，
//         因为执行体在检查点之后不会再继续。
// ============================================================================
#include "engine/TaskScheduler.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

namespace dc {

// ===========================================================================
//  TaskInfo
// ===========================================================================
qint64 TaskInfo::elapsedMs() const
{
    if (!startedAt.isValid())
        return 0;

    const QDateTime end = finishedAt.isValid() ? finishedAt : QDateTime::currentDateTime();
    return startedAt.msecsTo(end);
}

// ===========================================================================
//  TaskScheduler
// ===========================================================================
TaskScheduler::TaskScheduler(QObject *parent)
    : QObject(parent)
{
    // 并发上限 1：桌宠场景下绝大多数任务都有副作用，
    // 串行执行能让「用户看到的状态」与「实际发生的事」顺序一致，
    // 避免两个命令交叉执行导致的混乱提示。
    m_maxConcurrent = 1;
}

TaskScheduler::~TaskScheduler()
{
    cancelAll();
}

void TaskScheduler::setDefaultTimeoutMs(int ms)
{
    m_defaultTimeoutMs = qMax(0, ms);
}

int TaskScheduler::defaultTimeoutMs() const
{
    return m_defaultTimeoutMs;
}

void TaskScheduler::setMaxConcurrentTasks(int count)
{
    m_maxConcurrent = qMax(1, count);
}

int TaskScheduler::maxConcurrentTasks() const
{
    return m_maxConcurrent;
}

// ===========================================================================
//  提交
// ===========================================================================
QString TaskScheduler::submit(const TaskDefinition &definition)
{
    if (m_shutdown) {
        DC_WARN(LogCat::Core,
                QStringLiteral("调度器已停止，拒绝新任务：%1").arg(definition.title));
        return QString();
    }

    // 执行体为空是编程错误，直接拒绝而不是崩溃（SRS 7.2）
    if (!definition.execute) {
        DC_ERROR(LogCat::Core,
                 QStringLiteral("任务 %1 没有执行体，已拒绝。").arg(definition.ruleId));
        return QString();
    }

    InternalTask task;
    task.definition = definition;

    task.info.taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    task.info.ruleId = definition.ruleId;
    task.info.intent = definition.intent;
    task.info.title = definition.title;
    task.info.status = TaskStatus::Pending;
    task.info.createdAt = QDateTime::currentDateTime();
    task.info.timeoutMs = definition.timeoutMs > 0
        ? definition.timeoutMs
        : m_defaultTimeoutMs;

    const QString taskId = task.info.taskId;

    m_tasks.insert(taskId, task);
    m_pendingQueue.enqueue(taskId);

    DC_INFO(LogCat::Core,
            QStringLiteral("任务入队：%1（%2，超时 %3 ms）")
                .arg(task.info.title, task.info.intent)
                .arg(task.info.timeoutMs));

    emit taskQueued(task.info);

    processQueue();
    return taskId;
}

// ===========================================================================
//  队列处理
// ===========================================================================
void TaskScheduler::processQueue()
{
    while (!m_pendingQueue.isEmpty()) {
        // 并发控制：独占任务要求当前没有其它任务在跑
        const QString nextId = m_pendingQueue.head();

        if (!m_tasks.contains(nextId)) {
            m_pendingQueue.dequeue();   // 已被取消并移除
            continue;
        }

        const InternalTask &next = m_tasks.value(nextId);

        // 已请求取消的任务不进执行体，直接结算
        if (next.cancelRequested) {
            m_pendingQueue.dequeue();
            finishTask(nextId, TaskStatus::Cancelled,
                       erc::describe(erc::OperationCancelled),
                       QStringLiteral("任务在开始前被取消"));
            continue;
        }

        const int runningTotal = m_runningExclusive + m_runningShared;

        if (next.definition.exclusive) {
            // 独占任务：必须没有任何任务在运行
            if (runningTotal > 0)
                break;
            ++m_runningExclusive;
        } else {
            // 共享任务：受总并发上限约束
            if (runningTotal >= m_maxConcurrent)
                break;
            ++m_runningShared;
        }

        m_pendingQueue.dequeue();
        startTask(nextId);
    }
}

// ===========================================================================
//  启动任务
// ===========================================================================
void TaskScheduler::startTask(const QString &taskId)
{
    if (!m_tasks.contains(taskId))
        return;

    InternalTask &task = m_tasks[taskId];
    task.info.status = TaskStatus::Running;
    task.info.startedAt = QDateTime::currentDateTime();
    task.info.statusMessage = QStringLiteral("正在执行…");

    emit taskStarted(task.info);
    emit taskStatusChanged(task.info);

    // ---- 超时定时器 ----
    if (task.info.timeoutMs > 0) {
        const QString capturedId = taskId;
        QTimer::singleShot(task.info.timeoutMs, this, [this, capturedId]() {
            onTaskTimeout(capturedId);
        });
    }

    // ---- 在工作线程中执行 ----
    // 用 QtConcurrent 风格：这里直接用 QThreadPool + 回调
    // 注意：progress / cancelProbe 回调的内部实现必须线程安全地
    //       回到本对象（通过 QMetaObject::invokeMethod 排队到本线程）
    const QString capturedId = taskId;

    QThreadPool::globalInstance()->start([this, capturedId]() {
        runTaskBody(capturedId);
    });

    DC_DEBUG(LogCat::Core,
             QStringLiteral("任务开始：%1").arg(m_tasks.value(taskId).info.title));
}

// ===========================================================================
//  任务体执行（工作线程）
// ===========================================================================
void TaskScheduler::runTaskBody(const QString &taskId)
{
    // 取出执行体副本 —— 不在工作线程里访问 m_tasks 的引用，
    // 因为主线程可能同时修改它。
    TaskDefinition definition;
    {
        if (!m_tasks.contains(taskId))
            return;
        definition = m_tasks.value(taskId).definition;
    }

    // 进度回调：从工作线程切回主线程发信号
    auto progressCb = [this, taskId](int percent, const QString &message) {
        QMetaObject::invokeMethod(
            this,
            [this, taskId, percent, message]() {
                if (!m_tasks.contains(taskId))
                    return;
                m_tasks[taskId].info.progressPercent = percent;
                m_tasks[taskId].info.statusMessage = message;
                emit taskProgress(taskId, percent, message);
                emit taskStatusChanged(m_tasks.value(taskId).info);
            },
            Qt::QueuedConnection);
    };

    // 取消探针：读取取消标志。
    // 用原子读取语义即可（bool 在本场景下的可见性问题由
    // QMetaObject 队列调用的同步点覆盖），但为稳妥这里通过互斥保护。
    auto cancelProbe = [this, taskId]() -> bool {
        if (!m_tasks.contains(taskId))
            return true;

        // 注意：本函数的调用频率由执行体决定，因此这里的锁竞争可接受
        QMutexLocker locker(const_cast<QMutex *>(&m_taskMutex));
        return m_tasks.value(taskId).cancelRequested;
    };

    Result result;
    try {
        result = definition.execute(progressCb, cancelProbe);
    } catch (const std::exception &ex) {
        // 执行体抛异常：不要让异常穿透到线程池（会导致 terminate）
        result = Result::failure(erc::InternalError,
                                 QStringLiteral("任务执行时发生异常，该功能已跳过。"),
                                 QString::fromUtf8(ex.what()));
    } catch (...) {
        result = Result::failure(erc::InternalError,
                                 QStringLiteral("任务执行时发生未知异常，该功能已跳过。"));
    }

    // 回到主线程结算
    QMetaObject::invokeMethod(
        this,
        [this, taskId, result]() {
            if (!m_tasks.contains(taskId))
                return;

            // 并发计数回收
            const bool exclusive = m_tasks.value(taskId).definition.exclusive;
            if (exclusive) {
                m_runningExclusive = qMax(0, m_runningExclusive - 1);
            } else {
                m_runningShared = qMax(0, m_runningShared - 1);
            }

            if (m_tasks.value(taskId).cancelRequested) {
                finishTask(taskId, TaskStatus::Cancelled,
                           erc::describe(erc::OperationCancelled),
                           QStringLiteral("任务被用户取消"));
            } else if (result.ok) {
                finishTask(taskId, TaskStatus::Succeeded, QString(), result.detail);
            } else {
                finishTask(taskId, TaskStatus::Failed, result.message, result.detail);
            }

            processQueue();
        },
        Qt::QueuedConnection);
}

// ===========================================================================
//  完成结算
// ===========================================================================
void TaskScheduler::finishTask(const QString &taskId, TaskStatus status,
                              const QString &errorMessage, const QString &detail)
{
    if (!m_tasks.contains(taskId))
        return;

    InternalTask &task = m_tasks[taskId];

    // 已经是终态则不重复结算（防重复回调）
    if (task.info.isTerminal())
        return;

    task.info.status = status;
    task.info.finishedAt = QDateTime::currentDateTime();
    task.info.errorMessage = errorMessage;

    if (status == TaskStatus::Succeeded) {
        task.info.progressPercent = 100;
        task.info.statusMessage = QStringLiteral("已完成");
    } else if (status == TaskStatus::Cancelled) {
        task.info.statusMessage = QStringLiteral("已取消");
    } else if (status == TaskStatus::TimedOut) {
        task.info.statusMessage = QStringLiteral("已超时");
    } else {
        task.info.statusMessage = QStringLiteral("执行失败");
    }

    // 记录到最近任务列表
    m_recentOrder.prepend(taskId);
    while (m_recentOrder.size() > kMaxRecentTasks)
        m_recentOrder.removeLast();

    const TaskInfo snapshot = task.info;

    DC_INFO(LogCat::Core,
            QStringLiteral("任务结束：%1 -> %2（耗时 %3 ms）%4")
                .arg(snapshot.title,
                     taskStatusToString(status))
                .arg(snapshot.elapsedMs())
                .arg(detail.isEmpty() ? QString() : QStringLiteral(" | ") + detail));

    emit taskStatusChanged(snapshot);
    emit taskFinished(snapshot);
}

// ===========================================================================
//  超时处理
// ===========================================================================
void TaskScheduler::onTaskTimeout(const QString &taskId)
{
    if (!m_tasks.contains(taskId))
        return;

    InternalTask &task = m_tasks[taskId];

    // 已完成则忽略
    if (task.info.isTerminal())
        return;

    DC_WARN(LogCat::Core,
            QStringLiteral("任务超时：%1（%2 ms）")
                .arg(task.info.title).arg(task.info.timeoutMs));

    // 置取消标志，让执行体在下一个检查点退出。
    // 不强行 kill 线程 —— 强杀会留下半完成的副作用（例如写了一半的文件），
    // 比「稍后才真正停止」更危险。
    task.cancelRequested = true;

    // 尚未开始的直接结算
    if (task.info.status == TaskStatus::Pending) {
        m_pendingQueue.removeAll(taskId);
        finishTask(taskId, TaskStatus::TimedOut,
                   erc::describe(erc::Timeout),
                   QStringLiteral("任务在开始前超时"));
        processQueue();
        return;
    }

    // 正在运行的：标记为超时，等执行体自行退出后再统一结算。
    // 这里先按超时结算，避免 UI 长时间停在「正在执行」。
    finishTask(taskId, TaskStatus::TimedOut,
               erc::describe(erc::Timeout),
               QStringLiteral("任务执行超过 %1 ms").arg(task.info.timeoutMs));
}

// ===========================================================================
//  取消
// ===========================================================================
bool TaskScheduler::cancel(const QString &taskId)
{
    if (!m_tasks.contains(taskId))
        return false;

    InternalTask &task = m_tasks[taskId];

    if (task.info.isTerminal()) {
        DC_DEBUG(LogCat::Core,
                 QStringLiteral("任务已结束，无需取消：%1").arg(task.info.title));
        return false;
    }

    task.cancelRequested = true;

    DC_INFO(LogCat::Core,
            QStringLiteral("请求取消任务：%1").arg(task.info.title));

    // 尚未开始：立即从队列移除并结算
    if (task.info.status == TaskStatus::Pending) {
        m_pendingQueue.removeAll(taskId);
        finishTask(taskId, TaskStatus::Cancelled,
                   erc::describe(erc::OperationCancelled),
                   QStringLiteral("任务在开始前被取消"));
        processQueue();
    }
    // 正在运行：交由执行体的取消探针处理，其返回后会走结算流程

    return true;
}

void TaskScheduler::cancelAll()
{
    const QList<QString> ids = m_tasks.keys();
    for (const QString &id : ids)
        cancel(id);

    m_pendingQueue.clear();
}

bool TaskScheduler::waitForAll(int timeoutMs)
{
    const QDateTime deadline = QDateTime::currentDateTime().addMSecs(timeoutMs);

    while (QDateTime::currentDateTime() < deadline) {
        if (isIdle())
            return true;

        // 让事件循环继续处理，以便工作线程能通过队列连接回到主线程结算
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }

    return isIdle();
}

// ===========================================================================
//  查询
// ===========================================================================
TaskInfo TaskScheduler::taskInfo(const QString &taskId) const
{
    return m_tasks.value(taskId).info;
}

QVector<TaskInfo> TaskScheduler::activeTasks() const
{
    QVector<TaskInfo> out;
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it) {
        if (!it.value().info.isTerminal())
            out.append(it.value().info);
    }
    return out;
}

QVector<TaskInfo> TaskScheduler::recentTasks(int limit) const
{
    QVector<TaskInfo> out;
    const int count = qMin(limit, m_recentOrder.size());

    for (int i = 0; i < count; ++i) {
        const QString id = m_recentOrder.at(i);
        if (m_tasks.contains(id))
            out.append(m_tasks.value(id).info);
    }

    return out;
}

bool TaskScheduler::isIdle() const
{
    if (!m_pendingQueue.isEmpty())
        return false;

    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it) {
        if (!it.value().info.isTerminal())
            return false;
    }

    return true;
}

void TaskScheduler::shutdown()
{
    m_shutdown = true;
    cancelAll();

    DC_INFO(LogCat::Core, QStringLiteral("任务调度器已停止"));
}

bool TaskScheduler::isShutdown() const
{
    return m_shutdown;
}

void TaskScheduler::updateInfo(const QString &taskId)
{
    if (!m_tasks.contains(taskId))
        return;
    emit taskStatusChanged(m_tasks.value(taskId).info);
}

} // namespace dc
