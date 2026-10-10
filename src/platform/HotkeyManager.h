// ============================================================================
//  HotkeyManager.h — 全局快捷键注册与冲突检测
//
//  需求依据：SRS V1.1 第 4.12 节
//    默认快捷键：
//      Ctrl + Alt + Space  显示/隐藏桌宠或唤起输入框
//      Ctrl + Alt + M      切换麦克风输入状态
//      Ctrl + Alt + H      打开历史记录
//      Ctrl + Alt + L      打开命令输入框
//      Esc                 关闭当前气泡、弹窗或取消当前输入
//    「均应允许用户修改并检测冲突」
//
//  实现要点：
//    全局快捷键必须用平台原生 API（Windows: RegisterHotKey；
//    Linux: X11 XGrabKey）。Qt 本身不提供全局快捷键。
//    所有原生调用都收敛在 PlatformAdapter 之外的这个专门类里，
//    业务层只发信号、收信号。
//
//  兼容性（SRS 6.2）：
//    Wayland 下 XGrabKey 不可用 —— 此时 registered 为 false，
//    并给出明确原因，界面据此禁用快捷键设置区。
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace dc {

class PlatformAdapter;

// ---------------------------------------------------------------------------
// 单个快捷键的注册状态
// ---------------------------------------------------------------------------
struct HotkeyBinding {
    HotkeyAction action = HotkeyAction::TogglePet;
    QKeySequence sequence;          // 解析后的按键序列
    QString rawText;                // 用户配置的原始文本
    bool registered = false;
    QString failureReason;          // 注册失败原因（冲突/不支持/系统拒绝）

    bool isValid() const { return !sequence.isEmpty(); }
};

// ---------------------------------------------------------------------------
// 快捷键注册总结果
// ---------------------------------------------------------------------------
struct HotkeyRegistrationReport {
    bool globalHotkeySupported = false;   // 当前系统是否支持全局快捷键
    QString unsupportedReason;
    QVector<HotkeyBinding> bindings;

    int registeredCount() const;
    int failedCount() const;
    QStringList failureSummaries() const;
};

// ---------------------------------------------------------------------------
// HotkeyManager
// ---------------------------------------------------------------------------
class HotkeyManager : public QObject
{
    Q_OBJECT

public:
    explicit HotkeyManager(PlatformAdapter *adapter, QObject *parent = nullptr);
    ~HotkeyManager() override;

    // 当前平台是否支持全局快捷键（SRS 6.2）
    static bool isSupported(QString *reason = nullptr);

    // ---- 按键序列解析 ------------------------------------------------
    //  接受 "Ctrl+Alt+Space" / "ctrl+alt+m" / "Esc" 等写法。
    //  返回空序列表示无法解析（调用方应提示用户重新设置）。
    static QKeySequence parseSequence(const QString &text);

    //  规范化显示形式（"ctrl+alt+m" -> "Ctrl+Alt+M"）
    static QString canonicalText(const QKeySequence &sequence);

    // 静态冲突检测：解析一组配置文本，返回互相冲突的动作对
    //   SRS 4.12 要求「检测冲突」；配置加载时已做一次，
    //   这里再做一次是为了给界面提供「哪些冲突」的明细。
    static QVector<QPair<HotkeyAction, HotkeyAction>>
    detectConflicts(const QHash<HotkeyAction, QString> &configured);

    // ---- 注册 ---------------------------------------------------------
    // 按配置注册全部快捷键。已注册的会先注销。
    HotkeyRegistrationReport registerAll(const QHash<HotkeyAction, QString> &configured);

    // 单独注册/注销（用于设置页「立即试用」）
    HotkeyBinding registerOne(HotkeyAction action, const QString &text);
    void unregisterOne(HotkeyAction action);
    void unregisterAll();

    // 查询当前注册状态
    HotkeyRegistrationReport currentReport() const;
    bool isRegistered(HotkeyAction action) const;

    // 原生事件的入口：由 eventFilter / nativeEvent 调用
    // 返回 true 表示该热键已被本程序处理
    bool handleNativeHotkey(int nativeId);

signals:
    // 某个快捷键被按下
    void hotkeyTriggered(dc::HotkeyAction action);

    // 注册状态发生变化（用于刷新设置界面）
    void registrationChanged(const dc::HotkeyRegistrationReport &report);

private:
    // 平台专有实现
    bool registerNative(HotkeyAction action, const QKeySequence &sequence,
                        QString *failureReason);
    void unregisterNative(HotkeyAction action);

    PlatformAdapter *m_adapter = nullptr;
    QHash<HotkeyAction, HotkeyBinding> m_bindings;
    // 原生 id -> 动作 的映射（Windows RegisterHotKey 需要整型 id）
    QHash<int, HotkeyAction> m_idToAction;
    int m_nextId = 1;
    bool m_supported = false;
    QString m_unsupportedReason;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::HotkeyBinding)
Q_DECLARE_METATYPE(dc::HotkeyRegistrationReport)
