// ============================================================================
//  Types.h — 全局基础类型、枚举与常量
//
//  需求依据：SRS V1.1 第 3.3 节（核心模块）、第 4.4 节（风险等级）、
//            第 5 节（配置设计）
//
//  设计说明：
//    本文件是纯数据定义，不含任何业务逻辑，被各层共同依赖。
//    所有枚举都提供字符串互转函数，原因有二：
//      1) 配置/命令规则文件用字符串表达（可读、可手工编辑，符合 SRS 5）
//      2) 日志与历史记录需要可读的持久化表示（SRS 7.3）
// ============================================================================
#pragma once

#include <QString>
#include <QStringList>
#include <QMetaType>

namespace dc {

// ---------------------------------------------------------------------------
// 支持的语言（SRS 4.2 TXT-001：中文、英语、日语）
// ---------------------------------------------------------------------------
enum class Language {
    Auto = 0,   // 自动检测
    ZhCN,       // 简体中文
    EnUS,       // 英语
    JaJP        // 日语
};

QString languageToCode(Language lang);      // -> "zh-CN" / "en-US" / "ja-JP" / "auto"
Language languageFromCode(const QString &code);
QString languageDisplayName(Language lang);

// ---------------------------------------------------------------------------
// 风险等级（SRS 4.4 表格）
//   低   → 可按配置直接执行
//   中   → 执行前确认
//   高   → 明确授权；必要时由系统安全提示完成提升
//   禁止 → 不提供
// ---------------------------------------------------------------------------
enum class RiskLevel {
    Low = 0,
    Medium,
    High,
    Forbidden
};

QString riskLevelToString(RiskLevel level);
RiskLevel riskLevelFromString(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------------------
// 权限检查结论（SRS 3.4 PermissionManager::check 的返回值语义）
// ---------------------------------------------------------------------------
enum class PermissionVerdict {
    Allowed = 0,        // 允许执行（低风险，或已有覆盖本次操作的授权）
    NeedConfirm,        // 需要用户确认（中风险）
    NeedAuthorize,      // 需要用户明确授权（高风险）
    Denied              // 拒绝（禁止级，或用户此前已拒绝且不得重试）
};

QString permissionVerdictToString(PermissionVerdict verdict);

// ---------------------------------------------------------------------------
// 网络权限模式（SRS 4.9 表格）
// ---------------------------------------------------------------------------
enum class NetworkMode {
    Offline = 0,        // 完全离线：禁止所有应用主动网络请求
    AskEachTime,        // 按次询问
    AllowConfigured,    // 允许已配置服务（只放行用户启用的搜索/翻译服务）
    Custom              // 自定义：按服务、功能或域名配置规则
};

QString networkModeToString(NetworkMode mode);
NetworkMode networkModeFromString(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------------------
// 历史记录类别（SRS 4.10：聊天、翻译、搜索、任务历史可分别启用或关闭）
// ---------------------------------------------------------------------------
enum class HistoryCategory {
    Conversation = 0,
    Task,
    Translation,
    Search,
    Recording
};

QString historyCategoryToString(HistoryCategory category);
QString historyCategoryTableName(HistoryCategory category);

// ---------------------------------------------------------------------------
// 任务状态（SRS 3.3 TaskScheduler、4.10 task_history.status）
// ---------------------------------------------------------------------------
enum class TaskStatus {
    Pending = 0,
    Running,
    Succeeded,
    Failed,
    Cancelled,
    TimedOut,
    Denied             // 用户拒绝授权导致未执行
};

QString taskStatusToString(TaskStatus status);

// ---------------------------------------------------------------------------
// 桌宠动画状态（SRS 4.1 PET-005）
// ---------------------------------------------------------------------------
enum class PetState {
    Idle = 0,       // 待机
    Talking,        // 说话
    Working,        // 执行任务
    Success,        // 成功
    Failure,        // 失败
    Listening       // 监听麦克风（SRS 4.5 SPK-002 需要清晰的状态提示）
};

QString petStateToString(PetState state);
PetState petStateFromString(const QString &text, bool *ok = nullptr);

// ---------------------------------------------------------------------------
// 语音后端能力（SRS 4.5 SPK-007 / 6.2：能力探测控制功能入口）
// ---------------------------------------------------------------------------
struct SpeechCapability {
    bool recognitionAvailable = false;   // 是否具备语音识别后端
    bool synthesisAvailable   = false;   // 是否具备语音合成后端
    bool pushToTalkAvailable  = false;   // 是否支持按键式识别
    QString backendId;                   // 后端标识，如 "sapi"
    QString backendDisplayName;
    QString unavailableReason;           // 不可用原因，直接展示给用户（SRS 6.2）
};

// ---------------------------------------------------------------------------
// 全局快捷键动作（SRS 4.12）
// ---------------------------------------------------------------------------
enum class HotkeyAction {
    TogglePet = 0,      // Ctrl+Alt+Space 显示/隐藏桌宠或唤起输入框
    ToggleMic,          // Ctrl+Alt+M 切换麦克风输入状态
    OpenHistory,        // Ctrl+Alt+H 打开历史记录
    OpenInput,          // Ctrl+Alt+L 打开命令输入框
    Cancel              // Esc 关闭当前气泡、弹窗或取消当前输入
};

QString hotkeyActionToString(HotkeyAction action);
QString hotkeyActionDisplayName(HotkeyAction action);

// ---------------------------------------------------------------------------
// 通用结果结构
//   所有服务接口统一返回它，避免异常穿透模块边界（SRS 7.2：单个模块崩溃
//   不得导致核心退出，因此错误以返回值而非异常传递）
// ---------------------------------------------------------------------------
struct Result {
    bool ok = true;
    QString errorCode;      // 机器可读错误码，见 core/ErrorCode.h
    QString message;        // 面向用户的可理解说明（SRS 7.2）
    QString detail;         // 面向日志的技术细节（不得含敏感内容，SRS 7.3）

    static Result success() { return Result{true, {}, {}, {}}; }

    static Result failure(const QString &code, const QString &msg,
                          const QString &det = QString())
    {
        return Result{false, code, msg, det};
    }

    explicit operator bool() const { return ok; }
};

// ---------------------------------------------------------------------------
// 应用数据目录布局
//   集中定义，避免各处硬编码路径（同时便于环境隔离说明文档核对）
// ---------------------------------------------------------------------------
struct AppPaths {
    QString configDir;       // 配置文件目录
    QString dataDir;         // 数据库、录音等数据目录
    QString logDir;          // 日志目录
    QString cacheDir;        // 缓存（动画解码等）
    QString recordingsDir;   // 录音保存目录
    QString historyDbPath;   // SQLite 数据库完整路径
    QString configFilePath;  // 主配置文件完整路径
};

} // namespace dc

Q_DECLARE_METATYPE(dc::RiskLevel)
Q_DECLARE_METATYPE(dc::PermissionVerdict)
Q_DECLARE_METATYPE(dc::TaskStatus)
Q_DECLARE_METATYPE(dc::PetState)
Q_DECLARE_METATYPE(dc::Language)
