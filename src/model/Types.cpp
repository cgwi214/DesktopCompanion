// ============================================================================
//  Types.cpp — 基础类型字符串互转实现
//
//  需求依据：SRS 4.4（风险等级）、4.9（网络模式）、4.10（历史类别）、
//            4.12（快捷键）、4.2（语言）
// ============================================================================
#include "model/Types.h"

#include <QHash>

namespace dc {

// ---------------------------------------------------------------------------
// 语言
// ---------------------------------------------------------------------------
QString languageToCode(Language lang)
{
    switch (lang) {
    case Language::ZhCN: return QStringLiteral("zh-CN");
    case Language::EnUS: return QStringLiteral("en-US");
    case Language::JaJP: return QStringLiteral("ja-JP");
    case Language::Auto: break;
    }
    return QStringLiteral("auto");
}

Language languageFromCode(const QString &code)
{
    // 归一化后比较，兼容 zh_CN / ZH-cn / zh 等写法
    const QString normalized = code.trimmed().toLower().replace('_', '-');

    if (normalized.startsWith(QStringLiteral("zh")))
        return Language::ZhCN;
    if (normalized.startsWith(QStringLiteral("ja")))
        return Language::JaJP;
    if (normalized.startsWith(QStringLiteral("en")))
        return Language::EnUS;

    return Language::Auto;
}

QString languageDisplayName(Language lang)
{
    switch (lang) {
    case Language::ZhCN: return QStringLiteral("简体中文");
    case Language::EnUS: return QStringLiteral("English");
    case Language::JaJP: return QStringLiteral("日本語");
    case Language::Auto: break;
    }
    return QStringLiteral("自动检测");
}

// ---------------------------------------------------------------------------
// 风险等级
// ---------------------------------------------------------------------------
QString riskLevelToString(RiskLevel level)
{
    switch (level) {
    case RiskLevel::Low:       return QStringLiteral("low");
    case RiskLevel::Medium:    return QStringLiteral("medium");
    case RiskLevel::High:      return QStringLiteral("high");
    case RiskLevel::Forbidden: return QStringLiteral("forbidden");
    }
    return QStringLiteral("forbidden");   // 未知值一律按最严格处理（失败安全）
}

RiskLevel riskLevelFromString(const QString &text, bool *ok)
{
    static const QHash<QString, RiskLevel> table{
        { QStringLiteral("low"),       RiskLevel::Low },
        { QStringLiteral("medium"),    RiskLevel::Medium },
        { QStringLiteral("high"),      RiskLevel::High },
        { QStringLiteral("forbidden"), RiskLevel::Forbidden },
    };

    const QString key = text.trimmed().toLower();
    const auto it = table.constFind(key);
    if (it != table.constEnd()) {
        if (ok) *ok = true;
        return it.value();
    }

    if (ok) *ok = false;
    // 解析失败时返回最严格等级：宁可多问一次，也不静默放行（SRS 7.3）
    return RiskLevel::Forbidden;
}

QString permissionVerdictToString(PermissionVerdict verdict)
{
    switch (verdict) {
    case PermissionVerdict::Allowed:       return QStringLiteral("allowed");
    case PermissionVerdict::NeedConfirm:   return QStringLiteral("need_confirm");
    case PermissionVerdict::NeedAuthorize: return QStringLiteral("need_authorize");
    case PermissionVerdict::Denied:        return QStringLiteral("denied");
    }
    return QStringLiteral("denied");
}

// ---------------------------------------------------------------------------
// 网络模式
// ---------------------------------------------------------------------------
QString networkModeToString(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Offline:          return QStringLiteral("offline");
    case NetworkMode::AskEachTime:      return QStringLiteral("ask_each_time");
    case NetworkMode::AllowConfigured:  return QStringLiteral("allow_configured");
    case NetworkMode::Custom:           return QStringLiteral("custom");
    }
    return QStringLiteral("offline");
}

NetworkMode networkModeFromString(const QString &text, bool *ok)
{
    static const QHash<QString, NetworkMode> table{
        { QStringLiteral("offline"),          NetworkMode::Offline },
        { QStringLiteral("ask_each_time"),    NetworkMode::AskEachTime },
        { QStringLiteral("allow_configured"), NetworkMode::AllowConfigured },
        { QStringLiteral("custom"),           NetworkMode::Custom },
    };

    const QString key = text.trimmed().toLower();
    const auto it = table.constFind(key);
    if (it != table.constEnd()) {
        if (ok) *ok = true;
        return it.value();
    }

    if (ok) *ok = false;
    // 解析失败按最保守模式处理：完全离线（SRS 4.9：默认关闭未配置的网络服务）
    return NetworkMode::Offline;
}

// ---------------------------------------------------------------------------
// 历史类别
// ---------------------------------------------------------------------------
QString historyCategoryToString(HistoryCategory category)
{
    switch (category) {
    case HistoryCategory::Conversation: return QStringLiteral("conversation");
    case HistoryCategory::Task:         return QStringLiteral("task");
    case HistoryCategory::Translation:  return QStringLiteral("translation");
    case HistoryCategory::Search:       return QStringLiteral("search");
    case HistoryCategory::Recording:    return QStringLiteral("recording");
    }
    return QStringLiteral("conversation");
}

QString historyCategoryTableName(HistoryCategory category)
{
    // 表名与 SRS 4.10 给出的表名严格一致
    switch (category) {
    case HistoryCategory::Conversation: return QStringLiteral("conversation");
    case HistoryCategory::Task:         return QStringLiteral("task_history");
    case HistoryCategory::Translation:  return QStringLiteral("translation_history");
    case HistoryCategory::Search:       return QStringLiteral("search_history");
    case HistoryCategory::Recording:    return QStringLiteral("recording_metadata");
    }
    return QStringLiteral("conversation");
}

// ---------------------------------------------------------------------------
// 任务状态
// ---------------------------------------------------------------------------
QString taskStatusToString(TaskStatus status)
{
    switch (status) {
    case TaskStatus::Pending:   return QStringLiteral("pending");
    case TaskStatus::Running:   return QStringLiteral("running");
    case TaskStatus::Succeeded: return QStringLiteral("succeeded");
    case TaskStatus::Failed:    return QStringLiteral("failed");
    case TaskStatus::Cancelled: return QStringLiteral("cancelled");
    case TaskStatus::TimedOut:  return QStringLiteral("timed_out");
    case TaskStatus::Denied:    return QStringLiteral("denied");
    }
    return QStringLiteral("failed");
}

// ---------------------------------------------------------------------------
// 桌宠状态
// ---------------------------------------------------------------------------
QString petStateToString(PetState state)
{
    switch (state) {
    case PetState::Idle:      return QStringLiteral("idle");
    case PetState::Talking:   return QStringLiteral("talking");
    case PetState::Working:   return QStringLiteral("working");
    case PetState::Success:   return QStringLiteral("success");
    case PetState::Failure:   return QStringLiteral("failure");
    case PetState::Listening: return QStringLiteral("listening");
    }
    return QStringLiteral("idle");
}

PetState petStateFromString(const QString &text, bool *ok)
{
    static const QHash<QString, PetState> table{
        { QStringLiteral("idle"),      PetState::Idle },
        { QStringLiteral("talking"),   PetState::Talking },
        { QStringLiteral("working"),   PetState::Working },
        { QStringLiteral("success"),   PetState::Success },
        { QStringLiteral("failure"),   PetState::Failure },
        { QStringLiteral("listening"), PetState::Listening },
    };

    const QString key = text.trimmed().toLower();
    const auto it = table.constFind(key);
    if (it != table.constEnd()) {
        if (ok) *ok = true;
        return it.value();
    }

    if (ok) *ok = false;
    return PetState::Idle;
}

// ---------------------------------------------------------------------------
// 快捷键动作
// ---------------------------------------------------------------------------
QString hotkeyActionToString(HotkeyAction action)
{
    switch (action) {
    case HotkeyAction::TogglePet:   return QStringLiteral("toggle_pet");
    case HotkeyAction::ToggleMic:   return QStringLiteral("toggle_mic");
    case HotkeyAction::OpenHistory: return QStringLiteral("open_history");
    case HotkeyAction::OpenInput:   return QStringLiteral("open_input");
    case HotkeyAction::Cancel:      return QStringLiteral("cancel");
    }
    return QStringLiteral("cancel");
}

QString hotkeyActionDisplayName(HotkeyAction action)
{
    switch (action) {
    case HotkeyAction::TogglePet:   return QStringLiteral("显示/隐藏桌宠");
    case HotkeyAction::ToggleMic:   return QStringLiteral("切换麦克风输入");
    case HotkeyAction::OpenHistory: return QStringLiteral("打开历史记录");
    case HotkeyAction::OpenInput:   return QStringLiteral("打开命令输入框");
    case HotkeyAction::Cancel:      return QStringLiteral("取消当前操作");
    }
    return QString();
}

} // namespace dc
