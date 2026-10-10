// ============================================================================
//  tst_pet_state.cpp — 桌宠状态机单元测试
//
//  需求依据：SRS V1.1 4.1 PET-006（静态图只能做有限反馈）、7.1（低占用）
//
//  测试重点：
//    1. 状态转移：请求 -> 实际生效（requestState 不保证立刻切换）
//    2. 优先级：低优先级请求不能打断高优先级状态
//    3. 临时状态自动回落（Success/Failure -> Idle），且失败停留更久
//    4. tick() 的低占用语义：无变化时返回 false，界面不必重绘
//    5. 静态图模式下状态切换受限
//
//  第 4 条对应 SRS 7.1「避免持续轮询与重复开销」——
//  待机时不应该 60fps 空转重绘。
// ============================================================================
#include <QtTest/QtTest>

#include "model/Types.h"
#include "ui/PetStateMachine.h"

using namespace dc;

namespace {

// 造一个带基本动画配置的状态机
PetStateMachineConfig makeConfig()
{
    PetStateMachineConfig cfg;

    const QStringList names = {
        QStringLiteral("idle"),
        QStringLiteral("talking"),
        QStringLiteral("working"),
        QStringLiteral("success"),
        QStringLiteral("failure"),
        QStringLiteral("listening"),
    };

    for (const QString &n : names) {
        AnimationSpec spec;
        spec.name = n;
        spec.frameCount = 4;
        spec.frameIntervalMs = 200;
        spec.loop = (n == QStringLiteral("idle") || n == QStringLiteral("working"));
        cfg.animations.insert(n, spec);
    }

    return cfg;
}

} // namespace

class TestPetState : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // ---- 初始状态 ----
    void initial_stateIsIdle();

    // ---- 状态转移 ----
    void transition_requestIdleIsNoop();
    void transition_taskStartedGoesWorking();
    void transition_speakingGoesTalking();
    void transition_taskSuccessGoesSuccess();
    void transition_taskFailureGoesFailure();

    // ---- 优先级 ----
    void priority_listeningBeatsTalking();
    void priority_workingIsHigherThanIdle();
    void force_ignoresPriority();

    // ---- 临时状态回落 ----
    void hold_successFallsBackToIdle();
    void hold_failureStaysLongerThanSuccess();
    void transient_statesAreClassifiedCorrectly();

    // ---- tick 低占用 ----
    void tick_idleWithoutElapsedReturnsFalse();

    // ---- 静态图模式 ----
    void staticImage_limitsStateSwitching();

    // ---- 动画组名 ----
    void animation_currentNameMatchesState();
};

void TestPetState::init()
{
    // 每个用例用全新的状态机，避免相互影响
}

void TestPetState::cleanup()
{
}

// ---------------------------------------------------------------------------
// 初始状态
// ---------------------------------------------------------------------------

void TestPetState::initial_stateIsIdle()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    QCOMPARE(sm.state(), PetState::Idle);
}

// ---------------------------------------------------------------------------
// 状态转移
// ---------------------------------------------------------------------------

void TestPetState::transition_requestIdleIsNoop()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    // 已经是 Idle，再请求 Idle 不算「切换成功」
    const bool changed = sm.requestState(PetState::Idle);
    QCOMPARE(sm.state(), PetState::Idle);
    QVERIFY2(!changed, "重复请求同一状态不应报告为发生了切换");
}

void TestPetState::transition_taskStartedGoesWorking()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onTaskStarted();
    QCOMPARE(sm.state(), PetState::Working);
}

void TestPetState::transition_speakingGoesTalking()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onSpeakingStarted();
    QCOMPARE(sm.state(), PetState::Talking);

    sm.onSpeakingFinished();
    QCOMPARE(sm.state(), PetState::Idle);
}

void TestPetState::transition_taskSuccessGoesSuccess()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onTaskStarted();
    QCOMPARE(sm.state(), PetState::Working);

    sm.onTaskFinished(true);
    QCOMPARE(sm.state(), PetState::Success);
}

void TestPetState::transition_taskFailureGoesFailure()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onTaskStarted();
    sm.onTaskFinished(false);
    QCOMPARE(sm.state(), PetState::Failure);
}

// ---------------------------------------------------------------------------
// 优先级
// ---------------------------------------------------------------------------

// 正在听麦克风时，不应被「说话」打断 —— 监听还没结束说不上回话。
// 这是头文件里明确写出的设计意图。
void TestPetState::priority_listeningBeatsTalking()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onListeningStarted();
    QCOMPARE(sm.state(), PetState::Listening);

    // 尝试切到 Talking，应被拒绝
    const bool changed = sm.requestState(PetState::Talking);

    QVERIFY2(!changed, "Listening 状态下不应被 Talking 打断");
    QCOMPARE(sm.state(), PetState::Listening);
}

void TestPetState::priority_workingIsHigherThanIdle()
{
    // 优先级数值越高越抢占
    QVERIFY(PetStateMachine::statePriority(PetState::Working)
            > PetStateMachine::statePriority(PetState::Idle));
}

// forceState 用于「取消/打断」场景，必须无视优先级。
void TestPetState::force_ignoresPriority()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    sm.onListeningStarted();
    QCOMPARE(sm.state(), PetState::Listening);

    // 强制切到 Idle（模拟用户按 Esc 取消）
    sm.forceState(PetState::Idle);
    QCOMPARE(sm.state(), PetState::Idle);
}

// ---------------------------------------------------------------------------
// 临时状态回落
// ---------------------------------------------------------------------------

// Success 是临时状态，停留一会儿要自动回 Idle。
void TestPetState::hold_successFallsBackToIdle()
{
    PetStateMachineConfig cfg = makeConfig();
    cfg.successHoldMs = 100;   // 缩短便于测试
    cfg.animations[QStringLiteral("success")].frameIntervalMs = 10;

    PetStateMachine sm;
    sm.setConfig(cfg);

    sm.onTaskStarted();
    sm.onTaskFinished(true);
    QCOMPARE(sm.state(), PetState::Success);

    // 推进时间超过停留时长
    QElapsedTimer timer;
    timer.start();
    bool fellBack = false;
    while (timer.elapsed() < 2000) {
        sm.tick();
        if (sm.state() == PetState::Idle) {
            fellBack = true;
            break;
        }
        QTest::qWait(20);
    }

    QVERIFY2(fellBack, "Success 状态应在停留后自动回落到 Idle");
}

// 失败停留时间应比成功更久 —— 头文件注释写了「让用户看清原因」。
void TestPetState::hold_failureStaysLongerThanSuccess()
{
    PetStateMachineConfig cfg;
    // 用默认值验证配置语义
    QVERIFY2(cfg.failureHoldMs > cfg.successHoldMs,
             "失败状态的停留时长应大于成功状态（让用户看清失败原因）");
}

void TestPetState::transient_statesAreClassifiedCorrectly()
{
    // Success / Failure 是临时的（会自动回落）
    QVERIFY(PetStateMachine::isTransientState(PetState::Success));
    QVERIFY(PetStateMachine::isTransientState(PetState::Failure));

    // Idle 是稳态，不是临时的
    QVERIFY2(!PetStateMachine::isTransientState(PetState::Idle),
             "Idle 是稳态，不应被标记为临时状态");
}

// ---------------------------------------------------------------------------
// tick 低占用（SRS 7.1）
// ---------------------------------------------------------------------------

// 待机且帧计时器没到点时，tick() 应返回 false，
// 界面据此跳过重绘 —— 这是「低占用」的实现方式。
void TestPetState::tick_idleWithoutElapsedReturnsFalse()
{
    PetStateMachineConfig cfg = makeConfig();
    // 把 idle 的帧间隔设得很长，确保短时间内 tick 不产生变化
    cfg.animations[QStringLiteral("idle")].frameIntervalMs = 100000;

    PetStateMachine sm;
    sm.setConfig(cfg);

    QCOMPARE(sm.state(), PetState::Idle);

    const bool changed = sm.tick();
    QVERIFY2(!changed,
             "待机且帧间隔未到时 tick() 应返回 false，避免无谓重绘（SRS 7.1）");
}

// ---------------------------------------------------------------------------
// 静态图模式
// ---------------------------------------------------------------------------

// SRS 4.1 PET-006：用户用自定义静态图片时，只能做有限反馈。
void TestPetState::staticImage_limitsStateSwitching()
{
    PetStateMachineConfig cfg = makeConfig();
    cfg.staticImageMode = true;

    PetStateMachine sm;
    sm.setConfig(cfg);

    // 静态图模式下请求动画状态不应生效（仍停留在 Idle）
    sm.requestState(PetState::Talking);

    QVERIFY2(sm.state() == PetState::Idle,
             "静态图模式下不应切换到动画状态（PET-006：只能做有限反馈）");
}

// ---------------------------------------------------------------------------
// 动画组名
// ---------------------------------------------------------------------------

void TestPetState::animation_currentNameMatchesState()
{
    PetStateMachine sm;
    sm.setConfig(makeConfig());

    // Idle 时的动画组名应指向 idle
    QCOMPARE(sm.currentAnimationName(), QStringLiteral("idle"));

    sm.onSpeakingStarted();
    QCOMPARE(sm.currentAnimationName(), QStringLiteral("talking"));
}

QTEST_MAIN(TestPetState)
#include "tst_pet_state.moc"
