// ============================================================================
//  ConfigManager.cpp — 配置加载、校验、保存、迁移、导入导出
//
//  需求依据：SRS V1.1 第 5 节、7.2、7.3
// ============================================================================
#include "core/ConfigManager.h"

#include "core/ErrorCode.h"
#include "core/Logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStandardPaths>

#include <algorithm>

namespace dc {

// ===========================================================================
//  HotkeyConfig::forAction
// ===========================================================================
QString HotkeyConfig::forAction(HotkeyAction action) const
{
    switch (action) {
    case HotkeyAction::TogglePet:   return togglePet;
    case HotkeyAction::ToggleMic:   return toggleMic;
    case HotkeyAction::OpenHistory: return openHistory;
    case HotkeyAction::OpenInput:   return openInput;
    case HotkeyAction::Cancel:      return QStringLiteral("Esc");   // 固定，不提供配置
    }
    return QString();
}

// ===========================================================================
//  AppConfig -> PrivacySettings
// ===========================================================================
PrivacySettings AppConfig::toPrivacySettings() const
{
    PrivacySettings s;
    s.conversationEnabled = history.conversationEnabled;
    s.taskEnabled = history.taskEnabled;
    s.translationEnabled = history.translationEnabled;
    s.searchEnabled = history.searchEnabled;
    s.recordingEnabled = history.recordingEnabled;
    s.retentionDays = history.retentionDays;
    s.maxStorageBytes = history.maxStorageBytes;
    return s;
}

// ===========================================================================
//  JSON 读取辅助
//
//  全部走「带默认值与类型校验」的取值函数：
//    - 类型不符 -> 记一条 issue 并使用默认值，绝不抛异常
//    - 缺失字段 -> 静默使用默认值（这是正常的向前兼容行为）
// ===========================================================================
namespace {

ConfigIssue makeIssue(ConfigIssue::Severity severity, const QString &path,
                      const QString &message, const QString &fallback = QString())
{
    ConfigIssue issue;
    issue.severity = severity;
    issue.path = path;
    issue.message = message;
    issue.appliedFallback = fallback;
    return issue;
}

// --- 基础类型读取 ---------------------------------------------------------

bool readBool(const QJsonObject &obj, const QString &key, bool defaultValue,
              const QString &path, QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isBool())
        return v.toBool();

    // 容忍 "true"/"false" 字符串写法，但记录一条警告
    if (v.isString()) {
        const QString s = v.toString().trimmed().toLower();
        if (s == QStringLiteral("true") || s == QStringLiteral("1"))
            return true;
        if (s == QStringLiteral("false") || s == QStringLiteral("0"))
            return false;
    }

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望布尔值，实际为 %1，已使用默认值")
                                .arg(QString::fromUtf8(QJsonValue(v).type() == QJsonValue::Null
                                                           ? "null" : "其他类型")),
                            defaultValue ? QStringLiteral("true") : QStringLiteral("false")));
    return defaultValue;
}

int readInt(const QJsonObject &obj, const QString &key, int defaultValue,
            const QString &path, QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isDouble())
        return static_cast<int>(v.toDouble());

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望整数，已使用默认值"),
                            QString::number(defaultValue)));
    return defaultValue;
}

qint64 readInt64(const QJsonObject &obj, const QString &key, qint64 defaultValue,
                 const QString &path, QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isDouble())
        return static_cast<qint64>(v.toDouble());

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望整数，已使用默认值"),
                            QString::number(defaultValue)));
    return defaultValue;
}

double readDouble(const QJsonObject &obj, const QString &key, double defaultValue,
                  const QString &path, QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isDouble())
        return v.toDouble();

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望数值，已使用默认值"),
                            QString::number(defaultValue)));
    return defaultValue;
}

QString readString(const QJsonObject &obj, const QString &key,
                   const QString &defaultValue, const QString &path,
                   QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isString())
        return v.toString();

    // null 是合法写法，表示「未设置」
    if (v.isNull())
        return QString();

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望字符串，已使用默认值"), defaultValue));
    return defaultValue;
}

QStringList readStringList(const QJsonObject &obj, const QString &key,
                           const QStringList &defaultValue, const QString &path,
                           QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isNull())
        return QStringList();

    if (v.isArray()) {
        QStringList out;
        const QJsonArray arr = v.toArray();
        for (const QJsonValue &item : arr) {
            if (item.isString())
                out.append(item.toString());
            else
                issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                                        QStringLiteral("数组元素不是字符串，已跳过")));
        }
        return out;
    }

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望字符串数组，已使用默认值")));
    return defaultValue;
}

QMap<QString, QString> readStringMap(const QJsonObject &obj, const QString &key,
                                     const QMap<QString, QString> &defaultValue,
                                     const QString &path, QVector<ConfigIssue> &issues)
{
    if (!obj.contains(key))
        return defaultValue;

    const QJsonValue v = obj.value(key);
    if (v.isNull())
        return QMap<QString, QString>();

    if (v.isObject()) {
        QMap<QString, QString> out;
        const QJsonObject inner = v.toObject();
        for (auto it = inner.constBegin(); it != inner.constEnd(); ++it) {
            if (it.value().isString())
                out[it.key()] = it.value().toString();
            else
                issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                        path + QLatin1Char('.') + it.key(),
                                        QStringLiteral("映射值不是字符串，已跳过")));
        }
        return out;
    }

    issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                            QStringLiteral("期望字符串映射，已使用默认值")));
    return defaultValue;
}

// 小数范围钳制（SRS 5：启动时校验取值范围）
double clampDouble(double value, double lo, double hi)
{
    return std::max(lo, std::min(hi, value));
}

int clampInt(int value, int lo, int hi)
{
    return std::max(lo, std::min(hi, value));
}

} // namespace

// ===========================================================================
//  AppConfig::fromJson
// ===========================================================================
AppConfig AppConfig::fromJson(const QJsonObject &obj)
{
    QVector<ConfigIssue> ignored;
    return fromJsonWithIssues(obj, ignored);
}

// ===========================================================================
//  各段解析
// ===========================================================================
namespace {

UiConfig parseUi(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    UiConfig c;
    const QString p = QStringLiteral("ui");

    c.language = languageFromCode(
        readString(o, QStringLiteral("language"), languageToCode(c.language), p + QStringLiteral(".language"), issues));

    c.petSkinId = readString(o, QStringLiteral("pet_skin_id"), c.petSkinId, p + QStringLiteral(".pet_skin_id"), issues);
    c.petImagePath = readString(o, QStringLiteral("pet_image_path"), c.petImagePath, p + QStringLiteral(".pet_image_path"), issues);
    c.petAnimationDir = readString(o, QStringLiteral("pet_animation_dir"), c.petAnimationDir, p + QStringLiteral(".pet_animation_dir"), issues);
    c.animationIntervalMs = readInt(o, QStringLiteral("animation_interval_ms"), c.animationIntervalMs, p + QStringLiteral(".animation_interval_ms"), issues);
    c.alwaysOnTop = readBool(o, QStringLiteral("pet_always_on_top"), c.alwaysOnTop, p + QStringLiteral(".pet_always_on_top"), issues);
    c.opacity = readDouble(o, QStringLiteral("opacity"), c.opacity, p + QStringLiteral(".opacity"), issues);
    c.scale = readDouble(o, QStringLiteral("scale"), c.scale, p + QStringLiteral(".scale"), issues);
    c.posX = readInt(o, QStringLiteral("pos_x"), c.posX, p + QStringLiteral(".pos_x"), issues);
    c.posY = readInt(o, QStringLiteral("pos_y"), c.posY, p + QStringLiteral(".pos_y"), issues);
    c.startHidden = readBool(o, QStringLiteral("start_hidden"), c.startHidden, p + QStringLiteral(".start_hidden"), issues);
    c.bubbleDurationMs = readInt(o, QStringLiteral("bubble_duration_ms"), c.bubbleDurationMs, p + QStringLiteral(".bubble_duration_ms"), issues);
    c.bubbleMaxWidth = readInt(o, QStringLiteral("bubble_max_width"), c.bubbleMaxWidth, p + QStringLiteral(".bubble_max_width"), issues);
    c.showStatusHints = readBool(o, QStringLiteral("show_status_hints"), c.showStatusHints, p + QStringLiteral(".show_status_hints"), issues);
    c.trayIconEnabled = readBool(o, QStringLiteral("tray_icon_enabled"), c.trayIconEnabled, p + QStringLiteral(".tray_icon_enabled"), issues);

    return c;
}

StartupConfig parseStartup(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    StartupConfig c;
    const QString p = QStringLiteral("startup");
    c.enabled = readBool(o, QStringLiteral("enabled"), c.enabled, p + QStringLiteral(".enabled"), issues);
    c.startMinimized = readBool(o, QStringLiteral("start_minimized"), c.startMinimized, p + QStringLiteral(".start_minimized"), issues);
    c.delaySeconds = readInt(o, QStringLiteral("delay_seconds"), c.delaySeconds, p + QStringLiteral(".delay_seconds"), issues);
    c.autostartMethod = readString(o, QStringLiteral("autostart_method"), c.autostartMethod, p + QStringLiteral(".autostart_method"), issues);
    return c;
}

NetworkConfig parseNetwork(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    NetworkConfig c;
    const QString p = QStringLiteral("network");

    const QString modeText = readString(o, QStringLiteral("mode"),
                                        networkModeToString(c.mode),
                                        p + QStringLiteral(".mode"), issues);
    bool modeOk = false;
    c.mode = networkModeFromString(modeText, &modeOk);
    if (!modeOk) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning, p + QStringLiteral(".mode"),
                                QStringLiteral("未知网络模式「%1」，已按最保守的 offline 处理").arg(modeText),
                                QStringLiteral("offline")));
    }

    c.enabledSearchProvider = readString(o, QStringLiteral("enabled_search_provider"), c.enabledSearchProvider, p + QStringLiteral(".enabled_search_provider"), issues);
    c.enabledTranslationProvider = readString(o, QStringLiteral("enabled_translation_provider"), c.enabledTranslationProvider, p + QStringLiteral(".enabled_translation_provider"), issues);
    c.connectTimeoutMs = readInt(o, QStringLiteral("connect_timeout_ms"), c.connectTimeoutMs, p + QStringLiteral(".connect_timeout_ms"), issues);
    c.readTimeoutMs = readInt(o, QStringLiteral("read_timeout_ms"), c.readTimeoutMs, p + QStringLiteral(".read_timeout_ms"), issues);
    c.maxResponseBytes = readInt64(o, QStringLiteral("max_response_bytes"), c.maxResponseBytes, p + QStringLiteral(".max_response_bytes"), issues);
    c.maxRetries = readInt(o, QStringLiteral("max_retries"), c.maxRetries, p + QStringLiteral(".max_retries"), issues);
    c.allowedHosts = readStringList(o, QStringLiteral("allowed_hosts"), c.allowedHosts, p + QStringLiteral(".allowed_hosts"), issues);
    c.allowedServices = readStringList(o, QStringLiteral("allowed_services"), c.allowedServices, p + QStringLiteral(".allowed_services"), issues);
    c.askEachTimeRememberChoice = readBool(o, QStringLiteral("ask_each_time_remember_choice"), c.askEachTimeRememberChoice, p + QStringLiteral(".ask_each_time_remember_choice"), issues);

    // SRS 4.9：连接应使用 HTTPS/TLS；对证书错误不得静默忽略。
    // 若配置文件里被改成 true，这里强制改回 false 并记 Error 级问题（不是静默修正）。
    if (o.value(QStringLiteral("allow_insecure_tls")).toBool(false)) {
        issues.append(makeIssue(ConfigIssue::Severity::Error,
                                p + QStringLiteral(".allow_insecure_tls"),
                                QStringLiteral("不允许关闭 TLS 证书校验。该设置已被强制忽略。"),
                                QStringLiteral("false")));
    }
    c.allowInsecureTls = false;   // 恒为 false

    return c;
}

HistoryConfig parseHistory(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    HistoryConfig c;
    const QString p = QStringLiteral("history");
    c.conversationEnabled = readBool(o, QStringLiteral("conversation_enabled"), c.conversationEnabled, p + QStringLiteral(".conversation_enabled"), issues);
    c.taskEnabled = readBool(o, QStringLiteral("task_enabled"), c.taskEnabled, p + QStringLiteral(".task_enabled"), issues);
    c.translationEnabled = readBool(o, QStringLiteral("translation_enabled"), c.translationEnabled, p + QStringLiteral(".translation_enabled"), issues);
    c.searchEnabled = readBool(o, QStringLiteral("search_enabled"), c.searchEnabled, p + QStringLiteral(".search_enabled"), issues);
    c.recordingEnabled = readBool(o, QStringLiteral("recording_enabled"), c.recordingEnabled, p + QStringLiteral(".recording_enabled"), issues);
    c.retentionDays = readInt(o, QStringLiteral("retention_days"), c.retentionDays, p + QStringLiteral(".retention_days"), issues);
    c.maxStorageBytes = readInt64(o, QStringLiteral("max_storage_bytes"), c.maxStorageBytes, p + QStringLiteral(".max_storage_bytes"), issues);
    c.databasePath = readString(o, QStringLiteral("database_path"), c.databasePath, p + QStringLiteral(".database_path"), issues);

    // 密钥不随配置导出 —— 恒为 false
    if (o.value(QStringLiteral("export_includes_secrets")).toBool(false)) {
        issues.append(makeIssue(ConfigIssue::Severity::Error,
                                p + QStringLiteral(".export_includes_secrets"),
                                QStringLiteral("导出配置默认且必须排除密钥，该设置已被强制忽略。"),
                                QStringLiteral("false")));
    }
    c.exportIncludesSecrets = false;

    return c;
}

SpeechConfig parseSpeech(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    SpeechConfig c;
    const QString p = QStringLiteral("speech");

    c.enabled = readBool(o, QStringLiteral("enabled"), c.enabled, p + QStringLiteral(".enabled"), issues);
    c.recognitionLanguage = languageFromCode(
        readString(o, QStringLiteral("recognition_language"),
                   languageToCode(c.recognitionLanguage),
                   p + QStringLiteral(".recognition_language"), issues));
    c.pushToTalk = readBool(o, QStringLiteral("push_to_talk"), c.pushToTalk, p + QStringLiteral(".push_to_talk"), issues);
    c.backendId = readString(o, QStringLiteral("backend_id"), c.backendId, p + QStringLiteral(".backend_id"), issues);
    c.inputDeviceId = readString(o, QStringLiteral("input_device_id"), c.inputDeviceId, p + QStringLiteral(".input_device_id"), issues);
    c.outputDeviceId = readString(o, QStringLiteral("output_device_id"), c.outputDeviceId, p + QStringLiteral(".output_device_id"), issues);
    c.volumeThreshold = readDouble(o, QStringLiteral("volume_threshold"), c.volumeThreshold, p + QStringLiteral(".volume_threshold"), issues);
    c.recognitionTimeoutMs = readInt(o, QStringLiteral("recognition_timeout_ms"), c.recognitionTimeoutMs, p + QStringLiteral(".recognition_timeout_ms"), issues);
    c.minConfidence = readDouble(o, QStringLiteral("min_confidence"), c.minConfidence, p + QStringLiteral(".min_confidence"), issues);
    c.synthesisEnabled = readBool(o, QStringLiteral("synthesis_enabled"), c.synthesisEnabled, p + QStringLiteral(".synthesis_enabled"), issues);
    c.voiceId = readString(o, QStringLiteral("voice_id"), c.voiceId, p + QStringLiteral(".voice_id"), issues);
    c.speechRate = readInt(o, QStringLiteral("speech_rate"), c.speechRate, p + QStringLiteral(".speech_rate"), issues);
    c.speechVolume = readInt(o, QStringLiteral("speech_volume"), c.speechVolume, p + QStringLiteral(".speech_volume"), issues);

    // 录音（SRS 4.11），注意 recording_enabled 与 speech.enabled 相互独立
    c.recordingEnabled = readBool(o, QStringLiteral("recording_enabled"), c.recordingEnabled, p + QStringLiteral(".recording_enabled"), issues);
    c.recordingDirectory = readString(o, QStringLiteral("recording_directory"), c.recordingDirectory, p + QStringLiteral(".recording_directory"), issues);
    c.recordingFormat = readString(o, QStringLiteral("recording_format"), c.recordingFormat, p + QStringLiteral(".recording_format"), issues);
    c.recordingSampleRate = readInt(o, QStringLiteral("recording_sample_rate"), c.recordingSampleRate, p + QStringLiteral(".recording_sample_rate"), issues);
    c.recordingChannels = readInt(o, QStringLiteral("recording_channels"), c.recordingChannels, p + QStringLiteral(".recording_channels"), issues);
    c.recordingMaxDurationSec = readInt(o, QStringLiteral("recording_max_duration_sec"), c.recordingMaxDurationSec, p + QStringLiteral(".recording_max_duration_sec"), issues);
    c.recordingMaxStorageBytes = readInt64(o, QStringLiteral("recording_max_storage_bytes"), c.recordingMaxStorageBytes, p + QStringLiteral(".recording_max_storage_bytes"), issues);

    return c;
}

CommandConfig parseCommand(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    CommandConfig c;
    const QString p = QStringLiteral("command");
    c.minConfidence = readDouble(o, QStringLiteral("min_confidence"), c.minConfidence, p + QStringLiteral(".min_confidence"), issues);
    c.maxInputLength = readInt(o, QStringLiteral("max_input_length"), c.maxInputLength, p + QStringLiteral(".max_input_length"), issues);
    c.allowUserRules = readBool(o, QStringLiteral("allow_user_rules"), c.allowUserRules, p + QStringLiteral(".allow_user_rules"), issues);
    c.ruleFiles = readStringList(o, QStringLiteral("rule_files"), c.ruleFiles, p + QStringLiteral(".rule_files"), issues);

    // SRS 4.6：用户自定义命令不得直接执行任意拼接的 shell 字符串。
    // 若配置被改为 true，这里不阻止（提供了独立受控界面才允许），但必须记 Error
    // 并保证脚本执行路径仍需单独的权限确认。
    c.scriptsEnabled = readBool(o, QStringLiteral("scripts_enabled"), c.scriptsEnabled, p + QStringLiteral(".scripts_enabled"), issues);
    if (c.scriptsEnabled) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                p + QStringLiteral(".scripts_enabled"),
                                QStringLiteral("已启用脚本执行。脚本不会被自动执行，"
                                               "每次运行都会单独请求授权（SRS 4.6）。"),
                                QStringLiteral("true")));
    }
    return c;
}

FilesConfig parseFiles(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    FilesConfig c;
    const QString p = QStringLiteral("files");
    c.allowedDirectories = readStringList(o, QStringLiteral("allowed_directories"), c.allowedDirectories, p + QStringLiteral(".allowed_directories"), issues);
    c.pathAliases = readStringMap(o, QStringLiteral("path_aliases"), c.pathAliases, p + QStringLiteral(".path_aliases"), issues);
    c.appAliases = readStringMap(o, QStringLiteral("app_aliases"), c.appAliases, p + QStringLiteral(".app_aliases"), issues);
    c.appPaths = readStringMap(o, QStringLiteral("app_paths"), c.appPaths, p + QStringLiteral(".app_paths"), issues);
    c.confirmOverwrite = readBool(o, QStringLiteral("confirm_overwrite"), c.confirmOverwrite, p + QStringLiteral(".confirm_overwrite"), issues);
    c.confirmDelete = readBool(o, QStringLiteral("confirm_delete"), c.confirmDelete, p + QStringLiteral(".confirm_delete"), issues);

    // 关键安全开关：禁止白名单外访问。若被改为 true 则强制回退。
    if (o.value(QStringLiteral("allow_directory_traversal_outside_whitelist")).toBool(false)) {
        issues.append(makeIssue(ConfigIssue::Severity::Error,
                                p + QStringLiteral(".allow_directory_traversal_outside_whitelist"),
                                QStringLiteral("不允许关闭路径白名单限制，该设置已被强制忽略。"),
                                QStringLiteral("false")));
    }
    c.allowDirectoryTraversalOutsideWhitelist = false;   // 恒为 false

    return c;
}

HotkeyConfig parseHotkey(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    HotkeyConfig c;
    const QString p = QStringLiteral("hotkey");
    c.enabled = readBool(o, QStringLiteral("enabled"), c.enabled, p + QStringLiteral(".enabled"), issues);
    c.togglePet = readString(o, QStringLiteral("toggle_pet"), c.togglePet, p + QStringLiteral(".toggle_pet"), issues);
    c.toggleMic = readString(o, QStringLiteral("toggle_mic"), c.toggleMic, p + QStringLiteral(".toggle_mic"), issues);
    c.openHistory = readString(o, QStringLiteral("open_history"), c.openHistory, p + QStringLiteral(".open_history"), issues);
    c.openInput = readString(o, QStringLiteral("open_input"), c.openInput, p + QStringLiteral(".open_input"), issues);
    return c;
}

PrivacyConfig parsePrivacy(const QJsonObject &o, QVector<ConfigIssue> &issues)
{
    PrivacyConfig c;
    const QString p = QStringLiteral("privacy");
    c.auditLogEnabled = readBool(o, QStringLiteral("audit_log_enabled"), c.auditLogEnabled, p + QStringLiteral(".audit_log_enabled"), issues);
    c.storeApiKeysInFile = readBool(o, QStringLiteral("store_api_keys_in_file"), c.storeApiKeysInFile, p + QStringLiteral(".store_api_keys_in_file"), issues);

    // 三项恒为 false 的隐私底线，被改动即报错并回退
    if (o.value(QStringLiteral("log_full_paths")).toBool(false)) {
        issues.append(makeIssue(ConfigIssue::Severity::Error, p + QStringLiteral(".log_full_paths"),
                                QStringLiteral("日志默认不记录完整敏感路径，该设置已被强制忽略。"),
                                QStringLiteral("false")));
    }
    c.logFullPaths = false;

    if (o.value(QStringLiteral("telemetry_enabled")).toBool(false)) {
        issues.append(makeIssue(ConfigIssue::Severity::Error, p + QStringLiteral(".telemetry_enabled"),
                                QStringLiteral("本程序不采集也不上报任何遥测数据，该设置已被强制忽略。"),
                                QStringLiteral("false")));
    }
    c.telemetryEnabled = false;

    return c;
}

} // namespace

// ===========================================================================
//  AppConfig::fromJsonWithIssues
// ===========================================================================
AppConfig AppConfig::fromJsonWithIssues(const QJsonObject &obj,
                                        QVector<ConfigIssue> &issues)
{
    AppConfig c;
    c.configVersion = readInt(obj, QStringLiteral("config_version"), kCurrentConfigVersion,
                              QStringLiteral("config_version"), issues);

    c.ui      = parseUi(obj.value(QStringLiteral("ui")).toObject(), issues);
    c.startup = parseStartup(obj.value(QStringLiteral("startup")).toObject(), issues);
    c.network = parseNetwork(obj.value(QStringLiteral("network")).toObject(), issues);
    c.history = parseHistory(obj.value(QStringLiteral("history")).toObject(), issues);
    c.speech  = parseSpeech(obj.value(QStringLiteral("speech")).toObject(), issues);
    c.command = parseCommand(obj.value(QStringLiteral("command")).toObject(), issues);
    c.files   = parseFiles(obj.value(QStringLiteral("files")).toObject(), issues);
    c.hotkey  = parseHotkey(obj.value(QStringLiteral("hotkey")).toObject(), issues);
    c.privacy = parsePrivacy(obj.value(QStringLiteral("privacy")).toObject(), issues);

    // 逐段解析后再做一次统一的范围校验与钳制
    const QVector<ConfigIssue> validationIssues = ConfigManager::validate(c);
    issues.append(validationIssues);

    return c;
}

// ===========================================================================
//  序列化
// ===========================================================================
namespace {

QJsonObject uiToJson(const UiConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("language")] = languageToCode(c.language);
    o[QStringLiteral("pet_skin_id")] = c.petSkinId;
    o[QStringLiteral("pet_image_path")] = c.petImagePath;
    o[QStringLiteral("pet_animation_dir")] = c.petAnimationDir;
    o[QStringLiteral("animation_interval_ms")] = c.animationIntervalMs;
    o[QStringLiteral("pet_always_on_top")] = c.alwaysOnTop;
    o[QStringLiteral("opacity")] = c.opacity;
    o[QStringLiteral("scale")] = c.scale;
    o[QStringLiteral("pos_x")] = c.posX;
    o[QStringLiteral("pos_y")] = c.posY;
    o[QStringLiteral("start_hidden")] = c.startHidden;
    o[QStringLiteral("bubble_duration_ms")] = c.bubbleDurationMs;
    o[QStringLiteral("bubble_max_width")] = c.bubbleMaxWidth;
    o[QStringLiteral("show_status_hints")] = c.showStatusHints;
    o[QStringLiteral("tray_icon_enabled")] = c.trayIconEnabled;
    return o;
}

QJsonObject startupToJson(const StartupConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("enabled")] = c.enabled;
    o[QStringLiteral("start_minimized")] = c.startMinimized;
    o[QStringLiteral("delay_seconds")] = c.delaySeconds;
    if (!c.autostartMethod.isEmpty())
        o[QStringLiteral("autostart_method")] = c.autostartMethod;
    return o;
}

QJsonObject networkToJson(const NetworkConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("mode")] = networkModeToString(c.mode);
    o[QStringLiteral("enabled_search_provider")] = c.enabledSearchProvider;
    // 未启用时显式写 null，与 SRS 5 示例一致
    if (c.enabledTranslationProvider.isEmpty())
        o[QStringLiteral("enabled_translation_provider")] = QJsonValue(QJsonValue::Null);
    else
        o[QStringLiteral("enabled_translation_provider")] = c.enabledTranslationProvider;

    o[QStringLiteral("connect_timeout_ms")] = c.connectTimeoutMs;
    o[QStringLiteral("read_timeout_ms")] = c.readTimeoutMs;
    o[QStringLiteral("max_response_bytes")] = static_cast<double>(c.maxResponseBytes);
    o[QStringLiteral("max_retries")] = c.maxRetries;
    o[QStringLiteral("allow_insecure_tls")] = false;
    o[QStringLiteral("allowed_hosts")] = QJsonArray::fromStringList(c.allowedHosts);
    o[QStringLiteral("allowed_services")] = QJsonArray::fromStringList(c.allowedServices);
    o[QStringLiteral("ask_each_time_remember_choice")] = c.askEachTimeRememberChoice;
    return o;
}

QJsonObject historyToJson(const HistoryConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("conversation_enabled")] = c.conversationEnabled;
    o[QStringLiteral("task_enabled")] = c.taskEnabled;
    o[QStringLiteral("translation_enabled")] = c.translationEnabled;
    o[QStringLiteral("search_enabled")] = c.searchEnabled;
    o[QStringLiteral("recording_enabled")] = c.recordingEnabled;
    o[QStringLiteral("retention_days")] = c.retentionDays;
    o[QStringLiteral("max_storage_bytes")] = static_cast<double>(c.maxStorageBytes);
    if (!c.databasePath.isEmpty())
        o[QStringLiteral("database_path")] = c.databasePath;
    o[QStringLiteral("export_includes_secrets")] = false;
    return o;
}

QJsonObject speechToJson(const SpeechConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("enabled")] = c.enabled;
    o[QStringLiteral("recognition_language")] = languageToCode(c.recognitionLanguage);
    o[QStringLiteral("push_to_talk")] = c.pushToTalk;
    o[QStringLiteral("backend_id")] = c.backendId;
    o[QStringLiteral("input_device_id")] = c.inputDeviceId;
    o[QStringLiteral("output_device_id")] = c.outputDeviceId;
    o[QStringLiteral("volume_threshold")] = c.volumeThreshold;
    o[QStringLiteral("recognition_timeout_ms")] = c.recognitionTimeoutMs;
    o[QStringLiteral("min_confidence")] = c.minConfidence;
    o[QStringLiteral("synthesis_enabled")] = c.synthesisEnabled;
    o[QStringLiteral("voice_id")] = c.voiceId;
    o[QStringLiteral("speech_rate")] = c.speechRate;
    o[QStringLiteral("speech_volume")] = c.speechVolume;
    o[QStringLiteral("recording_enabled")] = c.recordingEnabled;
    o[QStringLiteral("recording_directory")] = c.recordingDirectory;
    o[QStringLiteral("recording_format")] = c.recordingFormat;
    o[QStringLiteral("recording_sample_rate")] = c.recordingSampleRate;
    o[QStringLiteral("recording_channels")] = c.recordingChannels;
    o[QStringLiteral("recording_max_duration_sec")] = c.recordingMaxDurationSec;
    o[QStringLiteral("recording_max_storage_bytes")] = static_cast<double>(c.recordingMaxStorageBytes);
    return o;
}

QJsonObject commandToJson(const CommandConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("min_confidence")] = c.minConfidence;
    o[QStringLiteral("max_input_length")] = c.maxInputLength;
    o[QStringLiteral("allow_user_rules")] = c.allowUserRules;
    o[QStringLiteral("rule_files")] = QJsonArray::fromStringList(c.ruleFiles);
    o[QStringLiteral("scripts_enabled")] = c.scriptsEnabled;
    return o;
}

QJsonObject filesToJson(const FilesConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("allowed_directories")] = QJsonArray::fromStringList(c.allowedDirectories);

    auto mapToJson = [](const QMap<QString, QString> &m) {
        QJsonObject out;
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            out[it.key()] = it.value();
        return out;
    };

    o[QStringLiteral("path_aliases")] = mapToJson(c.pathAliases);
    o[QStringLiteral("app_aliases")] = mapToJson(c.appAliases);
    o[QStringLiteral("app_paths")] = mapToJson(c.appPaths);
    o[QStringLiteral("allow_directory_traversal_outside_whitelist")] = false;
    o[QStringLiteral("confirm_overwrite")] = c.confirmOverwrite;
    o[QStringLiteral("confirm_delete")] = c.confirmDelete;
    return o;
}

QJsonObject hotkeyToJson(const HotkeyConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("enabled")] = c.enabled;
    o[QStringLiteral("toggle_pet")] = c.togglePet;
    o[QStringLiteral("toggle_mic")] = c.toggleMic;
    o[QStringLiteral("open_history")] = c.openHistory;
    o[QStringLiteral("open_input")] = c.openInput;
    return o;
}

QJsonObject privacyToJson(const PrivacyConfig &c)
{
    QJsonObject o;
    o[QStringLiteral("audit_log_enabled")] = c.auditLogEnabled;
    o[QStringLiteral("log_full_paths")] = false;
    o[QStringLiteral("store_api_keys_in_file")] = c.storeApiKeysInFile;
    o[QStringLiteral("telemetry_enabled")] = false;
    return o;
}

} // namespace

QJsonObject AppConfig::toJson(bool includeSecrets) const
{
    Q_UNUSED(includeSecrets)
    // 注意：本结构体中不存在任何密钥字段（密钥统一走 SecureStore），
    // 因此无论 includeSecrets 取值如何，导出内容都不含密钥。
    // 这从结构上保证了 SRS 5「密钥默认不随配置导出」。

    QJsonObject root;
    root[QStringLiteral("config_version")] = configVersion;
    root[QStringLiteral("ui")] = uiToJson(ui);
    root[QStringLiteral("startup")] = startupToJson(startup);
    root[QStringLiteral("network")] = networkToJson(network);
    root[QStringLiteral("history")] = historyToJson(history);
    root[QStringLiteral("speech")] = speechToJson(speech);
    root[QStringLiteral("command")] = commandToJson(command);
    root[QStringLiteral("files")] = filesToJson(files);
    root[QStringLiteral("hotkey")] = hotkeyToJson(hotkey);
    root[QStringLiteral("privacy")] = privacyToJson(privacy);
    return root;
}

// ===========================================================================
//  校验
// ===========================================================================
QVector<ConfigIssue> ConfigManager::validate(AppConfig &c)
{
    QVector<ConfigIssue> issues;

    auto fixDouble = [&issues](const QString &path, double &value,
                              double lo, double hi) {
        if (value < lo || value > hi) {
            const double original = value;
            value = clampDouble(value, lo, hi);
            issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                                    QStringLiteral("取值 %1 超出允许范围 [%2, %3]")
                                        .arg(original).arg(lo).arg(hi),
                                    QString::number(value)));
        }
    };

    auto fixInt = [&issues](const QString &path, int &value, int lo, int hi) {
        if (value < lo || value > hi) {
            const int original = value;
            value = clampInt(value, lo, hi);
            issues.append(makeIssue(ConfigIssue::Severity::Warning, path,
                                    QStringLiteral("取值 %1 超出允许范围 [%2, %3]")
                                        .arg(original).arg(lo).arg(hi),
                                    QString::number(value)));
        }
    };

    // ---- ui ----
    fixDouble(QStringLiteral("ui.opacity"), c.ui.opacity, 0.2, 1.0);
    fixDouble(QStringLiteral("ui.scale"), c.ui.scale, 0.5, 3.0);
    fixInt(QStringLiteral("ui.animation_interval_ms"), c.ui.animationIntervalMs, 40, 5000);
    fixInt(QStringLiteral("ui.bubble_duration_ms"), c.ui.bubbleDurationMs, 500, 60000);
    fixInt(QStringLiteral("ui.bubble_max_width"), c.ui.bubbleMaxWidth, 160, 1200);

    // 位置：-1 表示未设置，其余必须为非负
    if (c.ui.posX < -1) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning, QStringLiteral("ui.pos_x"),
                                QStringLiteral("位置无效，已改为自动选择"), QStringLiteral("-1")));
        c.ui.posX = -1;
    }
    if (c.ui.posY < -1) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning, QStringLiteral("ui.pos_y"),
                                QStringLiteral("位置无效，已改为自动选择"), QStringLiteral("-1")));
        c.ui.posY = -1;
    }

    // 语言不能是 Auto 作为界面语言：界面必须有确定语言
    if (c.ui.language == Language::Auto) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning, QStringLiteral("ui.language"),
                                QStringLiteral("界面语言不能为「自动检测」，已改为简体中文"),
                                QStringLiteral("zh-CN")));
        c.ui.language = Language::ZhCN;
    }

    // ---- startup ----
    fixInt(QStringLiteral("startup.delay_seconds"), c.startup.delaySeconds, 0, 300);

    // ---- network（SRS 4.9：必须配置合理的超时与上限）----
    fixInt(QStringLiteral("network.connect_timeout_ms"), c.network.connectTimeoutMs, 1000, 120000);
    fixInt(QStringLiteral("network.read_timeout_ms"), c.network.readTimeoutMs, 1000, 300000);
    // 最大重试次数上限设为 3：SRS 4.9 明确「不得无限重试」
    fixInt(QStringLiteral("network.max_retries"), c.network.maxRetries, 0, 3);
    if (c.network.maxResponseBytes <= 0) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                QStringLiteral("network.max_response_bytes"),
                                QStringLiteral("最大响应大小必须为正数，已改为 2 MB"),
                                QStringLiteral("2097152")));
        c.network.maxResponseBytes = 2LL * 1024 * 1024;
    } else if (c.network.maxResponseBytes > 64LL * 1024 * 1024) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                QStringLiteral("network.max_response_bytes"),
                                QStringLiteral("最大响应大小过大，已限制为 64 MB"),
                                QStringLiteral("67108864")));
        c.network.maxResponseBytes = 64LL * 1024 * 1024;
    }

    // AllowConfigured 模式必须至少启用一个服务，否则等于离线却仍显示为允许，
    // 会让用户困惑。这里给出 Info 级提示（不修改取值，尊重用户意图）。
    if (c.network.mode == NetworkMode::AllowConfigured
        && c.network.enabledSearchProvider.isEmpty()
        && c.network.enabledTranslationProvider.isEmpty()) {
        issues.append(makeIssue(ConfigIssue::Severity::Info,
                                QStringLiteral("network.mode"),
                                QStringLiteral("当前为「允许已配置服务」但未启用任何服务，"
                                               "实际效果等同于离线。"),
                                networkModeToString(c.network.mode)));
    }

    // ---- history ----
    // retentionDays <= 0 表示永久保留，是合法取值
    if (c.history.retentionDays > 3650) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                QStringLiteral("history.retention_days"),
                                QStringLiteral("保留期限过长，已限制为 3650 天"),
                                QStringLiteral("3650")));
        c.history.retentionDays = 3650;
    }
    if (c.history.maxStorageBytes <= 0) {
        issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                QStringLiteral("history.max_storage_bytes"),
                                QStringLiteral("最大占用空间必须为正数，已改为 512 MB"),
                                QStringLiteral("536870912")));
        c.history.maxStorageBytes = 512LL * 1024 * 1024;
    }

    // ---- speech ----
    fixDouble(QStringLiteral("speech.volume_threshold"), c.speech.volumeThreshold, 0.0, 1.0);
    fixDouble(QStringLiteral("speech.min_confidence"), c.speech.minConfidence, 0.0, 1.0);
    fixInt(QStringLiteral("speech.recognition_timeout_ms"), c.speech.recognitionTimeoutMs, 1000, 60000);
    fixInt(QStringLiteral("speech.speech_rate"), c.speech.speechRate, -10, 10);
    fixInt(QStringLiteral("speech.speech_volume"), c.speech.speechVolume, 0, 100);
    fixInt(QStringLiteral("speech.recording_sample_rate"), c.speech.recordingSampleRate,
           8000, 192000);
    fixInt(QStringLiteral("speech.recording_channels"), c.speech.recordingChannels, 1, 2);
    fixInt(QStringLiteral("speech.recording_max_duration_sec"),
           c.speech.recordingMaxDurationSec, 1, 3600);

    // 录音格式白名单：避免配置里写出无法编码的格式（SRS 4.11）
    {
        static const QStringList allowedFormats{
            QStringLiteral("wav"), QStringLiteral("flac"),
            QStringLiteral("ogg"), QStringLiteral("mp3")
        };
        if (!allowedFormats.contains(c.speech.recordingFormat.toLower())) {
            issues.append(makeIssue(ConfigIssue::Severity::Warning,
                                    QStringLiteral("speech.recording_format"),
                                    QStringLiteral("不支持的录音格式「%1」")
                                        .arg(c.speech.recordingFormat),
                                    QStringLiteral("wav")));
            c.speech.recordingFormat = QStringLiteral("wav");
        } else {
            c.speech.recordingFormat = c.speech.recordingFormat.toLower();
        }
    }

    // ---- command ----
    fixDouble(QStringLiteral("command.min_confidence"), c.command.minConfidence, 0.0, 1.0);
    fixInt(QStringLiteral("command.max_input_length"), c.command.maxInputLength, 8, 4096);

    // ---- hotkey ----
    // 冲突检测（SRS 4.12：应允许用户修改并检测冲突）
    {
        const QVector<QPair<QString, QString>> pairs{
            { QStringLiteral("hotkey.toggle_pet"),    c.hotkey.togglePet },
            { QStringLiteral("hotkey.toggle_mic"),    c.hotkey.toggleMic },
            { QStringLiteral("hotkey.open_history"),  c.hotkey.openHistory },
            { QStringLiteral("hotkey.open_input"),    c.hotkey.openInput },
        };

        for (int i = 0; i < pairs.size(); ++i) {
            const QString seqI = pairs.at(i).second.trimmed();
            if (seqI.isEmpty() || seqI.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0)
                continue;

            for (int j = i + 1; j < pairs.size(); ++j) {
                const QString seqJ = pairs.at(j).second.trimmed();
                if (seqJ.isEmpty())
                    continue;

                if (seqI.compare(seqJ, Qt::CaseInsensitive) == 0) {
                    issues.append(makeIssue(ConfigIssue::Severity::Error,
                                            pairs.at(j).first,
                                            QStringLiteral("快捷键与 %1 冲突（同为 %2），"
                                                           "该快捷键将无法注册")
                                                .arg(pairs.at(i).first, seqI)));
                }
            }
        }
    }

    return issues;
}

// ===========================================================================
//  迁移
// ===========================================================================
bool ConfigManager::migrate(QJsonObject &root, QVector<ConfigIssue> &issues)
{
    int version = root.value(QStringLiteral("config_version")).toInt(0);
    bool changed = false;

    // 缺失版本号：视为 v1（早期手写配置）
    if (version == 0) {
        version = 1;
        root[QStringLiteral("config_version")] = 1;
        issues.append(makeIssue(ConfigIssue::Severity::Info,
                                QStringLiteral("config_version"),
                                QStringLiteral("配置缺少版本号，已按 v1 处理"),
                                QStringLiteral("1")));
        changed = true;
    }

    if (version > kCurrentConfigVersion) {
        // 不猜测未来版本的字段含义，仅提示并以只读方式加载
        issues.append(makeIssue(ConfigIssue::Severity::Error,
                                QStringLiteral("config_version"),
                                QStringLiteral("配置版本 %1 高于程序支持的 %2，"
                                               "已按只读方式加载，未知字段将被忽略。")
                                    .arg(version).arg(kCurrentConfigVersion)));
        return changed;
    }

    // ---- v1 -> v2 ----
    // 变更内容（相对 SRS 5 的初版示例）：
    //   1. speech 段拆分出录音相关字段（SRS 4.11 要求录音与语音识别是独立开关）
    //   2. 新增 hotkey 段（SRS 4.12）
    //   3. 新增 files 段（SRS 4.3 允许目录与别名）
    //   4. 新增 privacy 段（SRS 4.10 / 7.3）
    //   5. history 段新增 task_enabled（五类历史各自可开关）
    if (version == 1) {
        if (!root.contains(QStringLiteral("hotkey"))) {
            root[QStringLiteral("hotkey")] = hotkeyToJson(HotkeyConfig{});
            issues.append(makeIssue(ConfigIssue::Severity::Info,
                                    QStringLiteral("hotkey"),
                                    QStringLiteral("已为旧配置补充默认快捷键设置")));
        }
        if (!root.contains(QStringLiteral("files"))) {
            root[QStringLiteral("files")] = filesToJson(FilesConfig{});
            issues.append(makeIssue(ConfigIssue::Severity::Info,
                                    QStringLiteral("files"),
                                    QStringLiteral("已为旧配置补充默认文件访问设置")));
        }
        if (!root.contains(QStringLiteral("privacy"))) {
            root[QStringLiteral("privacy")] = privacyToJson(PrivacyConfig{});
            issues.append(makeIssue(ConfigIssue::Severity::Info,
                                    QStringLiteral("privacy"),
                                    QStringLiteral("已为旧配置补充默认隐私设置")));
        }

        QJsonObject speech = root.value(QStringLiteral("speech")).toObject();
        if (!speech.contains(QStringLiteral("recording_enabled"))) {
            speech[QStringLiteral("recording_enabled")] = false;
            speech[QStringLiteral("recording_format")] = QStringLiteral("wav");
            speech[QStringLiteral("recording_sample_rate")] = 16000;
            speech[QStringLiteral("recording_max_duration_sec")] = 300;
            issues.append(makeIssue(ConfigIssue::Severity::Info,
                                    QStringLiteral("speech.recording_enabled"),
                                    QStringLiteral("已为旧配置补充录音开关（默认关闭）"),
                                    QStringLiteral("false")));
        }
        root[QStringLiteral("speech")] = speech;

        QJsonObject history = root.value(QStringLiteral("history")).toObject();
        if (!history.contains(QStringLiteral("task_enabled"))) {
            history[QStringLiteral("task_enabled")] = true;
            issues.append(makeIssue(ConfigIssue::Severity::Info,
                                    QStringLiteral("history.task_enabled"),
                                    QStringLiteral("已补充任务历史开关"),
                                    QStringLiteral("true")));
        }
        root[QStringLiteral("history")] = history;

        root[QStringLiteral("config_version")] = 2;
        version = 2;
        changed = true;

        issues.append(makeIssue(ConfigIssue::Severity::Info,
                                QStringLiteral("config_version"),
                                QStringLiteral("配置已从 v1 迁移到 v2"),
                                QStringLiteral("2")));
    }

    return changed;
}

// ===========================================================================
//  ConfigManager
// ===========================================================================
ConfigManager::ConfigManager(QObject *parent)
    : QObject(parent)
{
    m_paths = resolvePaths();
}

ConfigManager::~ConfigManager() = default;

// ---------------------------------------------------------------------------
//  路径解析
//
//  便携模式判定规则（SRS 8.4：旧系统可优先考虑便携包）：
//    1. 显式传入 portableDir
//    2. 程序目录下存在 portable.marker 文件
//    3. 程序目录下存在 config/ 且可在其中写入
//  否则使用系统标准数据目录
// ---------------------------------------------------------------------------
AppPaths ConfigManager::resolvePaths(const QString &portableDir)
{
    AppPaths paths;

    QString base;
    bool portable = false;

    if (!portableDir.isEmpty()) {
        base = portableDir;
        portable = true;
    } else {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (QFile::exists(appDir + QStringLiteral("/portable.marker"))) {
            base = appDir;
            portable = true;
        }
    }

    if (portable) {
        QDir dir(base);
        paths.configDir     = dir.absoluteFilePath(QStringLiteral("config"));
        paths.dataDir       = dir.absoluteFilePath(QStringLiteral("data"));
        paths.logDir        = dir.absoluteFilePath(QStringLiteral("logs"));
        paths.cacheDir      = dir.absoluteFilePath(QStringLiteral("cache"));
        paths.recordingsDir = dir.absoluteFilePath(QStringLiteral("recordings"));
    } else {
        // QStandardPaths 在不同系统上给出各自的标准位置：
        //   Windows -> %APPDATA% / %LOCALAPPDATA%
        //   Linux   -> ~/.config / ~/.local/share
        const QString configBase =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        const QString dataBase =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString cacheBase =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation);

        paths.configDir     = configBase;
        paths.dataDir       = dataBase;
        paths.logDir        = dataBase + QStringLiteral("/logs");
        paths.cacheDir      = cacheBase;
        paths.recordingsDir = dataBase + QStringLiteral("/recordings");
    }

    paths.historyDbPath    = paths.dataDir + QStringLiteral("/history.db");
    paths.configFilePath   = paths.configDir + QStringLiteral("/config.json");

    return paths;
}

void ConfigManager::setPaths(const AppPaths &paths)
{
    m_paths = paths;
}

AppPaths ConfigManager::paths() const
{
    return m_paths;
}

// ---------------------------------------------------------------------------
//  加载
// ---------------------------------------------------------------------------
ConfigLoadResult ConfigManager::load(const QString &explicitPath)
{
    ConfigLoadResult result;

    const QString path = explicitPath.isEmpty() ? m_paths.configFilePath : explicitPath;
    result.sourcePath = path;

    QFile file(path);
    if (!file.exists()) {
        // 首次运行：使用默认配置，并要求写盘以便用户看到可编辑的配置文件
        DC_INFO(LogCat::Config,
                QStringLiteral("配置文件不存在，使用默认配置：%1").arg(path));
        result.config = AppConfig{};
        result.usedDefaults = true;
        result.needsSave = true;
        result.issues.append(makeIssue(ConfigIssue::Severity::Info,
                                       QStringLiteral("config"),
                                       QStringLiteral("未找到配置文件，已使用默认配置")));
        m_config = result.config;
        m_loaded = true;
        return result;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        DC_ERROR(LogCat::Config,
                 QStringLiteral("无法读取配置文件：%1（%2）").arg(path, file.errorString()));
        result.config = AppConfig{};
        result.usedDefaults = true;
        result.issues.append(makeIssue(ConfigIssue::Severity::Error,
                                       QStringLiteral("config"),
                                       QStringLiteral("无法读取配置文件，已使用默认配置：%1")
                                           .arg(file.errorString())));
        m_config = result.config;
        m_loaded = true;
        return result;
    }

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // ---- 配置损坏：备份原文件 + 恢复默认（SRS 5）----
        DC_ERROR(LogCat::Config,
                 QStringLiteral("配置文件解析失败：%1（位置 %2）")
                     .arg(parseError.errorString()).arg(parseError.offset));

        result.wasCorrupted = true;
        result.backupPath = makeBackup(path);
        result.config = AppConfig{};
        result.usedDefaults = true;
        result.needsSave = true;
        result.issues.append(makeIssue(
            ConfigIssue::Severity::Error,
            QStringLiteral("config"),
            QStringLiteral("配置文件格式有误（%1），原文件已备份至 %2，已恢复为默认配置。")
                .arg(parseError.errorString(), result.backupPath),
            QStringLiteral("defaults")));

        m_config = result.config;
        m_loaded = true;
        return result;
    }

    QJsonObject root = doc.object();

    // ---- 迁移 ----
    QVector<ConfigIssue> migrationIssues;
    const bool migrated = migrate(root, migrationIssues);
    result.issues.append(migrationIssues);
    if (migrated)
        result.needsSave = true;

    // ---- 构建配置 ----
    result.config = AppConfig::fromJsonWithIssues(root, result.issues);
    result.loadedFromFile = true;

    // 迁移后仍无法确定版本 -> 说明版本过新
    if (result.config.configVersion > kCurrentConfigVersion)
        result.needsSave = false;   // 不覆盖用户的更高版本配置

    m_config = result.config;
    m_loaded = true;

    DC_INFO(LogCat::Config,
            QStringLiteral("配置已加载：%1（版本 %2，问题 %3 条）")
                .arg(path)
                .arg(result.config.configVersion)
                .arg(result.issues.size()));

    if (result.hasErrors()) {
        // 只输出路径与问题数量，不输出取值（避免敏感内容入日志，SRS 5）
        for (const ConfigIssue &issue : result.issues) {
            if (issue.severity == ConfigIssue::Severity::Error)
                DC_WARN(LogCat::Config,
                        QStringLiteral("配置错误项：%1 - %2").arg(issue.path, issue.message));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
//  保存
// ---------------------------------------------------------------------------
Result ConfigManager::save()
{
    if (m_paths.configFilePath.isEmpty())
        return Result::failure(erc::ConfigWriteFailed,
                               QStringLiteral("配置路径未设置。"));

    return writeJsonFile(m_paths.configFilePath, m_config.toJson(false));
}

Result ConfigManager::saveTo(const QString &path)
{
    return writeJsonFile(path, m_config.toJson(false));
}

Result ConfigManager::writeJsonFile(const QString &path, const QJsonObject &obj) const
{
    QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return Result::failure(erc::ConfigWriteFailed,
                               erc::describe(erc::ConfigWriteFailed),
                               QStringLiteral("无法创建目录：%1").arg(dir.absolutePath()));
    }

    // 原子写入：先写临时文件再替换，避免写入中断导致配置损坏（SRS 5 隐含要求）
    const QString tmpPath = path + QStringLiteral(".tmp");

    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return Result::failure(erc::ConfigWriteFailed,
                               erc::describe(erc::ConfigWriteFailed),
                               QStringLiteral("无法写入临时文件：%1").arg(tmp.errorString()));
    }

    const QJsonDocument doc(obj);
    tmp.write(doc.toJson(QJsonDocument::Indented));
    tmp.flush();
    tmp.close();

    // 替换（Windows 上 QFile::rename 不能覆盖已存在文件，先删除）
    if (QFile::exists(path) && !QFile::remove(path)) {
        QFile::remove(tmpPath);
        return Result::failure(erc::ConfigWriteFailed,
                               erc::describe(erc::ConfigWriteFailed),
                               QStringLiteral("无法替换原配置文件"));
    }

    if (!QFile::rename(tmpPath, path)) {
        QFile::remove(tmpPath);
        return Result::failure(erc::ConfigWriteFailed,
                               erc::describe(erc::ConfigWriteFailed),
                               QStringLiteral("无法重命名临时配置文件"));
    }

    DC_INFO(LogCat::Config, QStringLiteral("配置已保存：%1").arg(path));
    return Result::success();
}

// ---------------------------------------------------------------------------
//  访问
// ---------------------------------------------------------------------------
const AppConfig &ConfigManager::config() const
{
    return m_config;
}

void ConfigManager::setConfig(const AppConfig &config)
{
    m_config = config;
    emit configChanged();
}

// ---------------------------------------------------------------------------
//  导入 / 导出
// ---------------------------------------------------------------------------
Result ConfigManager::exportTo(const QString &path, bool includeSecrets)
{
    // 结构上就不可能导出密钥：AppConfig 不含密钥字段，
    // 密钥只存在于 SecureStore。这里的参数仅用于向调用方表达意图与留痕。
    if (includeSecrets) {
        DC_WARN(LogCat::Config,
                QStringLiteral("调用方请求导出密钥，但配置结构不含密钥字段，已忽略该请求。"));
    }

    Result r = writeJsonFile(path, m_config.toJson(false));
    if (!r.ok)
        return r;

    DC_INFO(LogCat::Config, QStringLiteral("配置已导出（不含密钥）：%1").arg(path));
    return Result::success();
}

ConfigLoadResult ConfigManager::importFrom(const QString &path)
{
    // 导入复用加载逻辑，但不改动当前生效配置，交由调用方决定是否采用
    ConfigLoadResult result;

    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        result.issues.append(makeIssue(ConfigIssue::Severity::Error,
                                       QStringLiteral("config"),
                                       QStringLiteral("无法打开要导入的配置文件")));
        result.config = m_config;
        return result;
    }

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.wasCorrupted = true;
        result.config = m_config;
        result.issues.append(makeIssue(ConfigIssue::Severity::Error,
                                       QStringLiteral("config"),
                                       QStringLiteral("导入文件格式有误：%1")
                                           .arg(parseError.errorString())));
        return result;
    }

    QJsonObject root = doc.object();
    migrate(root, result.issues);
    result.config = AppConfig::fromJsonWithIssues(root, result.issues);
    result.sourcePath = path;
    result.loadedFromFile = true;
    return result;
}

// ---------------------------------------------------------------------------
//  恢复默认
// ---------------------------------------------------------------------------
Result ConfigManager::resetToDefaults(bool keepBackup)
{
    if (keepBackup && QFile::exists(m_paths.configFilePath)) {
        const QString backup = makeBackup(m_paths.configFilePath);
        DC_INFO(LogCat::Config, QStringLiteral("恢复默认前已备份：%1").arg(backup));
    }

    m_config = AppConfig{};
    return save();
}

// ---------------------------------------------------------------------------
//  备份
// ---------------------------------------------------------------------------
QString ConfigManager::makeBackup(const QString &path)
{
    if (!QFile::exists(path))
        return QString();

    const QFileInfo info(path);
    const QString dir = info.absolutePath();
    const QString base = info.completeBaseName();
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));

    const QString backup =
        QDir(dir).absoluteFilePath(QStringLiteral("%1.corrupted.%2.json").arg(base, stamp));

    if (QFile::copy(path, backup))
        return backup;

    return QString();
}

QStringList ConfigManager::availableBackups() const
{
    QDir dir(m_paths.configDir);
    if (!dir.exists())
        return QStringList();

    const QStringList filters{ QStringLiteral("config.*.json"),
                               QStringLiteral("config.*.bak") };
    QStringList result;
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);
    for (const QFileInfo &fi : files)
        result.append(fi.absoluteFilePath());
    return result;
}

// ---------------------------------------------------------------------------
bool ConfigLoadResult::hasErrors() const
{
    for (const ConfigIssue &issue : issues) {
        if (issue.severity == ConfigIssue::Severity::Error)
            return true;
    }
    return false;
}

bool ConfigLoadResult::hasWarnings() const
{
    for (const ConfigIssue &issue : issues) {
        if (issue.severity == ConfigIssue::Severity::Warning)
            return true;
    }
    return false;
}

} // namespace dc
