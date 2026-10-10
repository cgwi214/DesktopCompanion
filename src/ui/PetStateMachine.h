// ============================================================================
//  PetStateMachine.h — 桌宠动画状态机
//
//  需求依据：SRS V1.1 4.1
//    PET-005：动画状态包括待机、说话、工作、成功、失败、监听
//    PET-006：用户自定义静态图片时，仅提供有限反馈（不假装有完整动画）
//
//  为什么把状态机独立成一个类（而不是塞进 PetWindow）：
//    1. 它是**纯逻辑** —— 输入是事件、输出是状态与动画帧索引，
//       不碰任何 QWidget。因此可以脱离界面做单元测试，
//       而"状态卡死在 Working"这类 bug 最容易出现在界面代码里。
//    2. PetWindow 只负责"把当前状态画出来"，
//       不负责"该处于哪个状态"，职责单一。
//
//  状态转移规则（对应 SRS 4.1 的六个状态）：
//
//      Idle ──收到指令/开始说话──> Talking
//      Idle ──任务开始执行──────> Working
//      Working ──任务成功────────> Success ──超时──> Idle
//      Working ──任务失败────────> Failure ──超时──> Idle
//      Idle ──按下说话键/开始识别─> Listening
//      Listening ──识别结束──────> Talking（回话）或 Idle
//      任意状态 ──被显式打断─────> Idle
//
//  关键设计：所有"自动回落到 Idle"都由**超时时长**驱动，
//  而不是由界面自己 setTimeout。这样：
//    - 同一时刻只有一个定时器在管状态（不会出现两个 setTimeout 打架）
//    - 单元测试可以直接调用 tick() 推进时间，不用真的等
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

namespace dc {

// ---------------------------------------------------------------------------
// 驱动的动画序列：每个状态对应一组帧文件名的"模板"
//
//  实际文件名由 PetRenderer 结合 skins 目录拼出来，
//  状态机只负责说"现在该播哪一组、第几帧"。
// ---------------------------------------------------------------------------
struct AnimationSpec {
    QString name;                 // 动画组名，如 "idle" / "talking"
    int frameCount = 1;           // 帧数；1 表示静态图
    int frameIntervalMs = 200;    // 帧间隔
    bool loop = true;             // 是否循环播放
};

// ---------------------------------------------------------------------------
// 状态机的配置
// ---------------------------------------------------------------------------
struct PetStateMachineConfig {
    // 各状态下动画的帧数（由资源决定，启动时由 PetRenderer 填进来）
    QMap<QString, AnimationSpec> animations;

    // 成功后停留在 Success 状态多久（毫秒）
    int successHoldMs = 1200;

    // 失败后停留在 Failure 状态多久
    int failureHoldMs = 2200;   // 失败比成功停留久一点，让用户看清原因

    // 说话状态在没有新内容后的保持时长
    int talkingHoldMs = 0;      // 0 表示由气泡自己决定何时结束

    // 帧间隔下限，防止用户把动画调得过快导致 CPU 占用升高
    // （SRS 7.1：低占用）
    int minFrameIntervalMs = 60;

    // 用户使用自定义静态图片时，是否还能切换状态
    //（SRS 4.1 PET-006：静态图只能做有限反馈）
    bool staticImageMode = false;
};

// ---------------------------------------------------------------------------
// PetStateMachine
// ---------------------------------------------------------------------------
class PetStateMachine : public QObject
{
    Q_OBJECT

public:
    explicit PetStateMachine(QObject *parent = nullptr);
    ~PetStateMachine() override;

    void setConfig(const PetStateMachineConfig &config);
    PetStateMachineConfig config() const;

    // 当前状态与当前帧
    PetState state() const;
    int currentFrame() const;
    QString currentAnimationName() const;

    // -----------------------------------------------------------------------
    // 状态切换
    //
    //  requestState() 是"请求"，不保证立刻生效：
    //  例如正在 Listening 时请求 Talking，会被忽略，
    //  因为监听还没结束就说不上"回话"。
    //  返回 true 表示真的切换了。
    // -----------------------------------------------------------------------
    bool requestState(PetState newState);

    // 强制切换，忽略优先级（用于"取消/打断"这类场景）
    void forceState(PetState newState);

    // 回到待机
    void goIdle();

    // -----------------------------------------------------------------------
    // 事件入口（供 PetWindow / IntentRouter 连接）
    // -----------------------------------------------------------------------
    // 任务开始执行
    void onTaskStarted();
    // 任务结束
    void onTaskFinished(bool success);
    // 开始说话（气泡弹出）
    void onSpeakingStarted();
    // 说话结束（气泡关闭）
    void onSpeakingFinished();
    // 开始监听麦克风
    void onListeningStarted();
    // 监听结束（识别成功与否都会结束）
    void onListeningFinished();

    // -----------------------------------------------------------------------
    // 时间推进
    //
    //  界面每秒调一次（或每帧调）即可。
    //  返回 true 表示"有变化，需要重绘"，界面据此决定是否 update()，
    //  避免在待机时仍然 60fps 重绘（SRS 7.1 低占用）。
    // -----------------------------------------------------------------------
    bool tick();

    // 当前状态是否是一个"临时的、会自动回落的"状态
    static bool isTransientState(PetState state);

    // 状态优先级：数值越大越"抢占"。用于判断请求能否覆盖当前状态。
    static int statePriority(PetState state);

signals:
    // 状态变化（界面据此切换动画组）
    void stateChanged(dc::PetState oldState, dc::PetState newState);

    // 帧推进（界面据此重绘）
    void frameAdvanced(int frameIndex);

    // 状态需要结束（界面可据此关闭气泡）
    void stateHoldFinished(dc::PetState state);

private:
    // 进入某状态的初始化
    void enterState(PetState newState);
    // 帧间隔（已做下限保护）
    int frameIntervalFor(PetState state) const;
    // 该状态应停留多久（0 = 由外部事件结束）
    int holdDurationFor(PetState state) const;
    // 内部：真正改状态并广播
    void applyState(PetState newState);

    PetStateMachineConfig m_config;
    PetState m_state = PetState::Idle;
    int m_currentFrame = 0;
    int m_frameElapsedMs = 0;
    int m_stateElapsedMs = 0;
    bool m_stateIsHeld = false;   // 当前状态是否在"停留计时"中

    // 真实经过时间的来源（单调时钟）。
    // tick() 靠它判断"帧间隔是否真的到了"，而不是假装每次调用都过了一个间隔。
    // 未启动时 isValid() 为 false，首次 tick() 用它来区分"第一次调用"。
    QElapsedTimer m_tickClock;

    // 记录进入 Listening/Working 之前的状态，
    // 用于"结束后该回到哪里"。
    // 例如从 Talking 进 Listening，监听结束应该回 Talking 而不是 Idle。
    PetState m_stateBeforeTransient = PetState::Idle;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::AnimationSpec)
Q_DECLARE_METATYPE(dc::PetStateMachineConfig)
