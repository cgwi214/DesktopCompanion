// ============================================================================
//  ConfigManager.h — 配置加载、校验、保存与版本迁移
//
//  需求依据：SRS V1.1 第 5 节（配置设计）
//    - 配置采用 JSON，带版本号并支持迁移
//    - 启动时校验类型、取值范围和必需字段
//    - 配置损坏时保留原文件备份并提供恢复默认选项
//    - 不在日志中输出 API 密钥、录音内容或完整敏感路径
//    - 支持导入、导出用户可移植配置；密钥默认不随配置导出
//
//  设计说明：
//    配置被建模为「强类型结构体 + JSON 双向映射」，而不是到处 QVariant 取值。
//    理由：SRS 7.4 要求为核心规则编写单元测试，强类型结构体才能被可靠断言。
//    所有取值都走带默认值的取值函数，任何缺失字段都不会导致崩溃。
// ============================================================================
#pragma once

#include "model/HistoryRecord.h"
#include "model/Types.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QMap>

namespace dc {

// ---------------------------------------------------------------------------
// 当前配置版本（用于迁移）
// ---------------------------------------------------------------------------
inline constexpr int kCurrentConfigVersion = 2;
inline constexpr int kOldestSupportedConfigVersion = 1;

// ---------------------------------------------------------------------------
// ui 段（SRS 5 示例）
// ---------------------------------------------------------------------------
struct UiConfig {
    Language language = Language::ZhCN;

    // 桌宠外观（SRS 4.1 PET-004）
    QString petSkinId = QStringLiteral("default");
    QString petImagePath;                 // 用户导入的静态图片
    QString petAnimationDir;              // 序列帧动画目录
    int animationIntervalMs = 200;        // 帧间隔
    bool alwaysOnTop = true;
    double opacity = 1.0;                 // 0.2 ~ 1.0
    double scale = 1.0;                   // 0.5 ~ 3.0
    int posX = -1;                        // -1 表示未设置，由程序选择默认位置
    int posY = -1;
    bool startHidden = false;

    // 气泡（SRS 4.1 PET-006）
    int bubbleDurationMs = 4000;
    int bubbleMaxWidth = 360;

    // 交互
    bool showStatusHints = true;
    bool trayIconEnabled = true;
};

// ---------------------------------------------------------------------------
// startup 段（SRS 4.12）
// ---------------------------------------------------------------------------
struct StartupConfig {
    bool enabled = false;
    bool startMinimized = true;
    int delaySeconds = 0;                 // 延迟启动
    QString autostartMethod;              // 留空表示自动选择（Windows 启动文件夹 / XDG Autostart）
};

// ---------------------------------------------------------------------------
// network 段（SRS 4.9）
// ---------------------------------------------------------------------------
struct NetworkConfig {
    NetworkMode mode = NetworkMode::AllowConfigured;
    QString enabledSearchProvider = QStringLiteral("default");
    QString enabledTranslationProvider;   // 空 = 未启用
    int connectTimeoutMs = 5000;
    int readTimeoutMs = 10000;
    qint64 maxResponseBytes = 2LL * 1024 * 1024;   // 最大响应大小
    int maxRetries = 1;                   // SRS 4.9：不得无限重试
    bool allowInsecureTls = false;        // 恒为 false 才安全；保留字段仅为显式拒绝
    QStringList allowedHosts;             // Custom 模式下的域名白名单
    QStringList allowedServices;          // Custom 模式下的服务白名单
    bool askEachTimeRememberChoice = false;
};

// ---------------------------------------------------------------------------
// history 段（SRS 4.10）
// ---------------------------------------------------------------------------
struct HistoryConfig {
    bool conversationEnabled = true;
    bool taskEnabled = true;
    bool translationEnabled = true;
    bool searchEnabled = true;
    bool recordingEnabled = true;

    int retentionDays = 180;              // <=0 表示永久保留
    qint64 maxStorageBytes = 512LL * 1024 * 1024;
    QString databasePath;                 // 留空使用默认位置
    bool exportIncludesSecrets = false;   // 恒为 false（SRS 5：密钥默认不随配置导出）
};

// ---------------------------------------------------------------------------
// speech 段（SRS 4.5）
// ---------------------------------------------------------------------------
struct SpeechConfig {
    bool enabled = false;                 // 默认关闭
    Language recognitionLanguage = Language::ZhCN;
    bool pushToTalk = true;
    QString backendId = QStringLiteral("auto");

    QString inputDeviceId;                // 空 = 系统默认设备
    QString outputDeviceId;
    double volumeThreshold = 0.02;        // 音量阈值（归一化）
    int recognitionTimeoutMs = 6000;
    double minConfidence = 0.55;          // 低于此值要求确认（SRS 4.5 SPK-003）

    bool synthesisEnabled = true;
    QString voiceId;                      // 空 = 系统默认语音
    int speechRate = 0;                   // -10 ~ 10
    int speechVolume = 100;               // 0 ~ 100

    // --- 录音（SRS 4.11，与语音识别是独立开关）---
    bool recordingEnabled = false;        // 默认关闭
    QString recordingDirectory;           // 空 = 使用默认数据目录
    QString recordingFormat = QStringLiteral("wav");
    int recordingSampleRate = 16000;
    int recordingChannels = 1;
    int recordingMaxDurationSec = 300;    // 最长时长
    qint64 recordingMaxStorageBytes = 256LL * 1024 * 1024;  // 空间上限
};

// ---------------------------------------------------------------------------
// command 段
// ---------------------------------------------------------------------------
struct CommandConfig {
    double minConfidence = 0.6;           // 低于该值请求澄清（SRS 4.6）
    int maxInputLength = 512;             // 输入长度上限，防滥用（SRS 7.3）
    bool allowUserRules = true;
    QStringList ruleFiles;                // 附加规则文件（相对 resources/commands）
    bool scriptsEnabled = false;          // SRS 4.6：用户自定义命令不得直接执行 shell 拼接
};

// ---------------------------------------------------------------------------
// files 段（SRS 4.3）
// ---------------------------------------------------------------------------
struct FilesConfig {
    QStringList allowedDirectories;       // 允许访问的目录（白名单）
    QMap<QString, QString> pathAliases;   // 文件/目录别名 -> 路径
    QMap<QString, QString> appAliases;    // 应用别名 -> 应用 id
    QMap<QString, QString> appPaths;      // 应用 id -> 可执行文件路径
    bool allowDirectoryTraversalOutsideWhitelist = false;  // 恒为 false
    bool confirmOverwrite = true;         // SRS 4.3：覆盖必须单独确认
    bool confirmDelete = true;            // SRS 4.3：删除必须单独确认
};

// ---------------------------------------------------------------------------
// hotkey 段（SRS 4.12）
// ---------------------------------------------------------------------------
struct HotkeyConfig {
    QString togglePet    = QStringLiteral("Ctrl+Alt+Space");
    QString toggleMic    = QStringLiteral("Ctrl+Alt+M");
    QString openHistory  = QStringLiteral("Ctrl+Alt+H");
    QString openInput    = QStringLiteral("Ctrl+Alt+L");
    bool enabled = true;

    QString forAction(HotkeyAction action) const;
};

// ---------------------------------------------------------------------------
// privacy 段（SRS 4.10 / 7.3）
// ---------------------------------------------------------------------------
struct PrivacyConfig {
    bool auditLogEnabled = true;          // 重要操作审计
    bool logFullPaths = false;            // 恒为 false（SRS 5）
    bool storeApiKeysInFile = false;      // false 时走系统安全存储
    bool telemetryEnabled = false;        // 恒为 false：本程序不上报任何数据
};

// ---------------------------------------------------------------------------
// 校验问题
//
//  声明在 AppConfig 之前，因为 AppConfig::fromJsonWithIssues 需要它。
//  （两个结构相互无关，纯属编译顺序需要。）
// ---------------------------------------------------------------------------
struct ConfigIssue {
    enum class Severity { Info, Warning, Error };
    Severity severity = Severity::Warning;
    QString path;      // 如 "ui.opacity"
    QString message;
    QString appliedFallback;   // 实际采用了什么值
};

// ---------------------------------------------------------------------------
// 顶层配置
// ---------------------------------------------------------------------------
struct AppConfig {
    int configVersion = kCurrentConfigVersion;

    UiConfig ui;
    StartupConfig startup;
    NetworkConfig network;
    HistoryConfig history;
    SpeechConfig speech;
    CommandConfig command;
    FilesConfig files;
    HotkeyConfig hotkey;
    PrivacyConfig privacy;

    qint64 loadedAtMs = 0;

    QJsonObject toJson(bool includeSecrets = false) const;
    static AppConfig fromJson(const QJsonObject &obj);

    // 从 JSON 构建配置，同时把所有问题（字段类型错误、越界、被强制关闭的
    // 不安全开关等）收集到 issues 里。
    //
    //  为什么问题要"收集"而不是"抛异常"或"默默修掉"：
    //    1. 抛异常会让一个拼错的字段导致整个程序起不来；
    //    2. 默默修掉用户会以为设置生效了，实际没有 —— 这更糟。
    //    所以策略是：尽量修复，但每一处修复都记一条 issue，
    //    由界面汇总展示给用户（"有 3 项设置已被调整"）。
    static AppConfig fromJsonWithIssues(const QJsonObject &obj,
                                        QVector<ConfigIssue> &issues);

    PrivacySettings toPrivacySettings() const;
};

// ---------------------------------------------------------------------------
// 加载结果
// ---------------------------------------------------------------------------
struct ConfigLoadResult {
    AppConfig config;                 // 无论是否出错，始终是可用的配置对象
    QVector<ConfigIssue> issues;
    bool loadedFromFile = false;
    bool usedDefaults = false;
    bool wasCorrupted = false;
    QString backupPath;               // 损坏时生成的备份文件路径
    QString sourcePath;
    bool needsSave = false;           // 迁移或修复后需要回写

    bool hasErrors() const;
    bool hasWarnings() const;
};

// ---------------------------------------------------------------------------
// ConfigManager
// ---------------------------------------------------------------------------
class ConfigManager : public QObject
{
    Q_OBJECT

public:
    explicit ConfigManager(QObject *parent = nullptr);
    ~ConfigManager() override;

    // ---- 路径管理 --------------------------------------------------------
    // 计算所有应用目录。portableDir 非空时启用便携模式（配置与数据放在程序目录下），
    // 这是 SRS 8.4「旧系统可优先考虑便携包」要求的支撑点。
    static AppPaths resolvePaths(const QString &portableDir = QString());

    void setPaths(const AppPaths &paths);
    AppPaths paths() const;

    // ---- 加载 / 保存 -----------------------------------------------------
    // 永不抛出：任何失败都体现为 ConfigLoadResult 中的默认配置 + issues
    ConfigLoadResult load(const QString &explicitPath = QString());
    Result save();
    Result saveTo(const QString &path);

    // ---- 访问 ------------------------------------------------------------
    const AppConfig &config() const;
    void setConfig(const AppConfig &config);

    // ---- 导入 / 导出（SRS 5）--------------------------------------------
    // 导出时密钥默认剥离（除非显式要求，且界面需二次确认）
    Result exportTo(const QString &path, bool includeSecrets = false);
    ConfigLoadResult importFrom(const QString &path);

    // ---- 恢复默认 --------------------------------------------------------
    Result resetToDefaults(bool keepBackup = true);

    // ---- 备份管理 --------------------------------------------------------
    QStringList availableBackups() const;

    // ---- 校验（公开以便单测）--------------------------------------------
    static QVector<ConfigIssue> validate(AppConfig &config);

    // ---- 迁移（公开以便单测）--------------------------------------------
    // 把任意旧版本 JSON 就地升级到当前版本；返回是否发生了迁移
    static bool migrate(QJsonObject &root, QVector<ConfigIssue> &issues);

signals:
    void configChanged();
    void issuesReported(const QVector<dc::ConfigIssue> &issues);

private:
    Result writeJsonFile(const QString &path, const QJsonObject &obj) const;
    static QString makeBackup(const QString &path);

    AppPaths m_paths;
    AppConfig m_config;
    bool m_loaded = false;
};

} // namespace dc

Q_DECLARE_METATYPE(dc::ConfigIssue)
