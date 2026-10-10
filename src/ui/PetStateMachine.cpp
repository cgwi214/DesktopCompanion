// ============================================================================
//  PetStateMachine.cpp — 桌宠动画状态机实现
//
//  需求依据：SRS V1.1 4.1（PET-005 / PET-006）、7.1（低占用）
//
//  设计要点回顾（详见头文件注释）：
//    - 纯逻辑，不碰界面，可单元测试
//    - 所有"自动回落到 Idle"统一由 hold 计时驱动，只有一个定时源
//    - tick() 返回布尔值，界面靠它决定是否重绘 —— 待机时不重绘
// ============================================================================
#include "ui/PetStateMachine.h"

#include "core/Logger.h"

#include <QTimer>

namespace dc {

// ---------------------------------------------------------------------------
// 静态工具
// ---------------------------------------------------------------------------
bool PetStateMachine::isTransientState(PetState state)
{
    // 这三个状态是"过程性"的：它们会因为超时或不满足条件自动结束。
    // Idle 是稳态；Listening 虽然也是过程，但它由用户行为结束，
    // 不由内部计时器结束，所以不算 transient。
    switch (state) {
    case PetState::Success:
    case PetState::Failure:
        return true;
    case PetState::Idle:
    case PetState::Talking:
    case PetState::Working:
    case PetState::Listening:
        return false;
    }
    return false;
}

int PetStateMachine::statePriority(PetState state)
{
    // 优先级设计依据：
    //   用户能直接感知的反馈（成功/失败）应该能打断背景状态；
    //   监听中不能被"说话"打断（否则用户会看到麦克风还开着但桌宠在说话，
    //   不知道到底有没有在听）；
    //   工作状态比说话高：正在执行任务时不要被一句闲聊打断动画。
    switch (state) {
    case PetState::Idle:      return 0;
    case PetState::Talking:   return 10;
    case PetState::Success:   return 20;
    case PetState::Failure:   return 20;
    case PetState::Working:   return 30;
    case PetState::Listening: return 40;   // 最高：麦克风占用期间不被打断
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------
PetStateMachine::PetStateMachine(QObject *parent)
    : QObject(parent)
{
    // 默认动画规格。资源缺失时也能工作 ——
    // 每个状态都退化成"1 帧静态图"，只是不会有动画效果。
    const auto addSpec = [this](const QString &name, int frames, bool loop) {
        AnimationSpec spec;
        spec.name = name;
        spec.frameCount = frames;
        spec.frameIntervalMs = 200;
        spec.loop = loop;
        m_config.animations.insert(name, spec);
    };

    addSpec(QStringLiteral("idle"),      4, true);
    addSpec(QStringLiteral("talking"),   3, true);
    addSpec(QStringLiteral("working"),   4, true);
    addSpec(QStringLiteral("success"),   3, false);
    addSpec(QStringLiteral("failure"),   3, false);
    addSpec(QStringLiteral("listening"), 4, true);
}

PetStateMachine::~PetStateMachine() = default;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------
void PetStateMachine::setConfig(const PetStateMachineConfig &config)
{
    m_config = config;

    // 配置里的帧间隔可能被设得过小，会拉高 CPU。
    // 这里统一做一次下限保护，而不是在每个用到的对方各护一遍。
    for (auto it = m_config.animations.begin(); it != m_config.animations.end(); ++it) {
        if (it->frameIntervalMs < m_config.minFrameIntervalMs) {
            it->frameIntervalMs = m_config.minFrameIntervalMs;
        }
        if (it->frameCount < 1) {
            it->frameCount = 1;
        }
    }

    // 配置换了以后当前帧可能越界，夹一下
    const int frames = m_config.animations.value(currentAnimationName()).frameCount;
    if (m_currentFrame >= frames) {
        m_currentFrame = 0;
        emit frameAdvanced(m_currentFrame);
    }
}

PetStateMachineConfig PetStateMachine::config() const
{
    return m_config;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------
PetState PetStateMachine::state() const
{
    return m_state;
}

int PetStateMachine::currentFrame() const
{
    return m_currentFrame;
}

QString PetStateMachine::currentAnimationName() const
{
    switch (m_state) {
    case PetState::Idle:      return QStringLiteral("idle");
    case PetState::Talking:   return QStringLiteral("talking");
    case PetState::Working:   return QStringLiteral("working");
    case PetState::Success:   return QStringLiteral("success");
    case PetState::Failure:   return QStringLiteral("failure");
    case PetState::Listening: return QStringLiteral("listening");
    }
    return QStringLiteral("idle");
}

// ---------------------------------------------------------------------------
// 状态切换
// ---------------------------------------------------------------------------
bool PetStateMachine::requestState(PetState newState)
{
    if (newState == m_state) {
        return false;   // 同一状态重复请求，不算切换
    }

    // 静态图模式下只允许"有反馈意义"的切换。
    //
    //  SRS 4.1 PET-006：用户自定义静态图片时只提供有限反馈。
    //  为什么这么限制：静态图切来切去只会让用户困惑
    //  （图片明明没变，为什么状态变了），不如明确只做 Success/Failure 的提示。
    if (m_config.staticImageMode) {
        const bool allowed = (newState == PetState::Success
                              || newState == PetState::Failure
                              || newState == PetState::Idle);
        if (!allowed) {
            return false;
        }
    }

    // 优先级判断：低优先级不能覆盖高优先级
    if (statePriority(newState) < statePriority(m_state)) {
        // 特例：Success/Failure 是"结果反馈"，即使优先级不高于 Working，
        // 也应该能在 Working 之后自然发生 —— 但那时 Working 已经结束了。
        // 所以这里不需要额外放行。
        DC_INFO(LogCat::Ui,
                QStringLiteral("桌宠状态切换被忽略：%1 -> %2（优先级不足）")
                    .arg(petStateToString(m_state), petStateToString(newState)));
        return false;
    }

    applyState(newState);
    return true;
}

void PetStateMachine::forceState(PetState newState)
{
    applyState(newState);
}

void PetStateMachine::goIdle()
{
    if (m_state == PetState::Idle) {
        return;
    }
    applyState(PetState::Idle);
}

void PetStateMachine::applyState(PetState newState)
{
    const PetState oldState = m_state;
    m_state = newState;

    // 记录"从哪个稳态来的"。
    // 只记稳态（Idle / Talking），因为从 Working 或 Listening 这种
    // 过程状态进入另一个过程状态时，结束后应该回 Idle 而不是回到中间态。
    if (oldState == PetState::Idle || oldState == PetState::Talking) {
        m_stateBeforeTransient = oldState;
    }

    m_currentFrame = 0;
    m_frameElapsedMs = 0;
    m_stateElapsedMs = 0;
    m_stateIsHeld = isTransientState(newState);

    DC_INFO(LogCat::Ui,
            QStringLiteral("桌宠状态：%1 -> %2")
                .arg(petStateToString(oldState), petStateToString(newState)));

    emit stateChanged(oldState, newState);
    emit frameAdvanced(m_currentFrame);
}

// ---------------------------------------------------------------------------
// 事件入口
// ---------------------------------------------------------------------------
void PetStateMachine::onTaskStarted()
{
    requestState(PetState::Working);
}

void PetStateMachine::onTaskFinished(bool success)
{
    // 任务结束是「结果事件」，不是「竞争状态」。
    //
    // 这里曾经有一个 bug：直接调用 requestState(Success/Failure)，
    // 而 Success/Failure 的优先级（20）低于 Working（30）——
    // 于是任务结束时状态机正处在 Working，切换被优先级规则挡掉，
    // 成功/失败动画**永远不会播放**。
    //
    // 现场表现：用户说「打开浏览器」，浏览器确实打开了，但桌宠一直保持
    // 「执行中」的表情，不给任何成功反馈；用户会以为任务卡住了或者失败了。
    //
    // 关键在于：结果反馈不是「要不要抢占正在显示的另一个状态」的问题，
    // 而是「正在执行的任务已经结束」—— Working 本身已经不再成立，
    // 所以必须先离开 Working，再进入结果状态。用 forceState 表达这个语义
    // 比放宽优先级规则更准确：放宽规则会让任何高优先级状态都能被
    // Success 打断，那是另一回事（例如 Listening 期间不该被打断）。
    forceState(success ? PetState::Success : PetState::Failure);
}

void PetStateMachine::onSpeakingStarted()
{
    requestState(PetState::Talking);
}

void PetStateMachine::onSpeakingFinished()
{
    // 只在确实处于 Talking 时才回落。
    // 如果说话期间进入了 Working（例如"打开浏览器"边说话边执行），
    // 这里不能把它拉回 Idle —— 任务还在跑。
    if (m_state == PetState::Talking) {
        goIdle();
    }
}

void PetStateMachine::onListeningStarted()
{
    // Listening 优先级最高，直接抢占
    forceState(PetState::Listening);
}

void PetStateMachine::onListeningFinished()
{
    if (m_state != PetState::Listening) {
        return;   // 已经被别的逻辑改掉了，不做处理
    }

    // 回到进入监听之前的稳态（通常是 Talking 或 Idle）。
    // 这样"待机时按说话键 -> 识别 -> 回话"的流程看起来是连贯的。
    const PetState back = (m_stateBeforeTransient == PetState::Talking)
                              ? PetState::Talking
                              : PetState::Idle;
    applyState(back);
}

// ---------------------------------------------------------------------------
// 时间推进
// ---------------------------------------------------------------------------
bool PetStateMachine::tick()
{
    bool needsRedraw = false;

    // ---- 先算出「距离上次 tick 真正过了多久」----
    //
    // 这里曾经有一个 bug：旧实现每调用一次 tick() 就把累加器**固定加上一个
    // 帧间隔**（m_frameElapsedMs += frameInterval），于是"累加值 >= 帧间隔"
    // 这个条件第一次 tick 就必然成立 —— tick() 永远返回 true，
    // 界面每帧都在重绘，SRS 7.1「待机时低占用」的设计目标完全落空。
    // 现象是：桌宠待机时 CPU 占用居高不下，但画面上看不出任何变化。
    //
    // 正确做法是读取真实的经过时间。QElapsedTimer 走的是单调时钟，
    // 不受系统时间被调整的影响；界面以固定频率调用 tick()，
    // 累加器只在真实时间流逝时增长，于是「帧间隔没到就返回 false」成立。
    //
    // 注意首次调用：m_tickClock 尚未启动，elapsed() 返回 0，
    // 必须启动计时器并直接返回 false —— 否则第一次就会误判为"该推进帧了"。
    if (!m_tickClock.isValid()) {
        m_tickClock.start();
        return false;
    }

    const qint64 elapsedMs = m_tickClock.restart();
    if (elapsedMs <= 0) {
        return false;
    }

    const AnimationSpec spec = m_config.animations.value(currentAnimationName());
    const int frameInterval = qMax(spec.frameIntervalMs, m_config.minFrameIntervalMs);

    // ---- 帧推进 ----
    m_frameElapsedMs += static_cast<int>(elapsedMs);

    if (spec.frameCount > 1) {
        if (m_frameElapsedMs >= frameInterval) {
            m_frameElapsedMs = 0;
            ++m_currentFrame;

            if (m_currentFrame >= spec.frameCount) {
                if (spec.loop) {
                    m_currentFrame = 0;
                } else {
                    // 不循环的动画停在最后一帧（而不是跳回第一帧），
                    // 这样 Success/Failure 的"表情"能保持住。
                    m_currentFrame = spec.frameCount - 1;
                }
            }

            needsRedraw = true;
            emit frameAdvanced(m_currentFrame);
        }
    }

    // ---- hold 计时（Success / Failure 自动回落）----
    if (m_stateIsHeld) {
        const int holdMs = holdDurationFor(m_state);
        if (holdMs > 0) {
            m_stateElapsedMs += static_cast<int>(elapsedMs);

            if (m_stateElapsedMs >= holdMs) {
                m_stateIsHeld = false;
                emit stateHoldFinished(m_state);

                DC_INFO(LogCat::Ui,
                        QStringLiteral("桌宠状态停留结束，回落：%1")
                            .arg(petStateToString(m_state)));

                goIdle();
                needsRedraw = true;
            }
        }
    }

    // 静态图模式且是 Idle：完全不需要重绘。
    // 这是"低占用"的关键 —— 待机时 CPU 占用接近 0。
    if (m_config.staticImageMode && m_state == PetState::Idle) {
        needsRedraw = false;
    }

    return needsRedraw;
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------
int PetStateMachine::frameIntervalFor(PetState state) const
{
    Q_UNUSED(state)
    const AnimationSpec spec = m_config.animations.value(currentAnimationName());
    return qMax(spec.frameIntervalMs, m_config.minFrameIntervalMs);
}

int PetStateMachine::holdDurationFor(PetState state) const
{
    switch (state) {
    case PetState::Success: return m_config.successHoldMs;
    case PetState::Failure: return m_config.failureHoldMs;
    case PetState::Talking: return m_config.talkingHoldMs;   // 默认 0 = 由气泡结束
    default:                return 0;                        // 不由内部计时结束
    }
}

} // namespace dc
