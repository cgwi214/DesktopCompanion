// ============================================================================
//  ErrorCode.h — 统一错误码
//
//  需求依据：SRS V1.1
//    7.2 稳定性：网络超时、设备断开和文件不存在均应有可理解的错误处理
//    5   配置要求：配置损坏时保留原文件备份并提供恢复默认选项
//
//  设计说明：
//    错误码是「机器可读」的稳定标识，与之配套的 message 才是给用户看的。
//    这样做的好处是：
//      1) 单元测试可以断言错误码，不受文案调整影响
//      2) 界面可以按错误码切换到对应的引导动作（如「去设置网络」按钮）
// ============================================================================
#pragma once

#include "model/Types.h"

#include <QString>

namespace dc {
namespace erc {

// ---- 通用 ----------------------------------------------------------------
inline const char *Ok                = "DC-0000";
inline const char *InvalidArgument   = "DC-0001";
inline const char *NotInitialized    = "DC-0002";
inline const char *OperationCancelled= "DC-0003";
inline const char *Timeout           = "DC-0004";
inline const char *InternalError     = "DC-0005";
inline const char *NotSupported      = "DC-0006";
inline const char *OperationBlocked  = "DC-0007";  // 安全策略主动拦截（非错误，是设计行为）
inline const char *FileNotFound      = "DC-0008";
inline const char *NotImplemented    = "DC-0009";
inline const char *Busy              = "DC-000A";
inline const char *AlreadyExists     = "DC-000B";

// ---- 配置 DC-1xxx --------------------------------------------------------
inline const char *ConfigNotFound        = "DC-1001";
inline const char *ConfigParseError      = "DC-1002";
inline const char *ConfigValidationError = "DC-1003";
inline const char *ConfigVersionTooNew   = "DC-1004";
inline const char *ConfigMigrationFailed = "DC-1005";
inline const char *ConfigWriteFailed     = "DC-1006";
inline const char *ConfigBackupCreated   = "DC-1007";

// ---- 命令引擎 DC-2xxx ----------------------------------------------------
inline const char *CommandNoMatch        = "DC-2001";
inline const char *CommandAmbiguous      = "DC-2002";
inline const char *CommandLowConfidence  = "DC-2003";
inline const char *CommandRuleConflict   = "DC-2004";
inline const char *CommandInvalidInput   = "DC-2005";
inline const char *CommandParamMissing   = "DC-2006";
inline const char *CommandParamInvalid   = "DC-2007";

// ---- 权限 DC-3xxx --------------------------------------------------------
inline const char *PermissionDenied          = "DC-3001";
inline const char *PermissionNeedConfirm     = "DC-3002";
inline const char *PermissionNeedAuthorize   = "DC-3003";
inline const char *PermissionForbidden       = "DC-3004";
inline const char *PermissionPreviouslyDenied= "DC-3005";
inline const char *PermissionElevationRefused= "DC-3006";

// ---- 路径与文件 DC-4xxx --------------------------------------------------
inline const char *PathEmpty             = "DC-4001";
inline const char *PathTraversalDetected = "DC-4002";
inline const char *PathNotAllowed        = "DC-4003";
inline const char *PathNotFound          = "DC-4004";
inline const char *PathNotAccessible     = "DC-4005";
inline const char *PathSensitiveLocation = "DC-4006";
inline const char *PathOverwriteRequired = "DC-4007";
inline const char *PathIsDirectory       = "DC-4008";
inline const char *PathInvalidCharacters = "DC-4009";

// ---- 应用启动 DC-5xxx ----------------------------------------------------
inline const char *AppIdNotFound     = "DC-5001";
inline const char *AppPathMissing    = "DC-5002";
inline const char *AppStartFailed    = "DC-5003";
inline const char *AppArgsRejected   = "DC-5004";

// ---- 网络 DC-6xxx --------------------------------------------------------
inline const char *NetworkOffline            = "DC-6001";
inline const char *NetworkNotConfigured      = "DC-6002";
inline const char *NetworkUserRefused        = "DC-6003";
inline const char *NetworkProviderDisabled   = "DC-6004";
inline const char *NetworkTlsError           = "DC-6005";
inline const char *NetworkCertificateInvalid = "DC-6006";
inline const char *NetworkTimeout            = "DC-6007";
inline const char *NetworkResponseTooLarge   = "DC-6008";
inline const char *NetworkHostNotAllowed     = "DC-6009";
inline const char *NetworkInsecureScheme     = "DC-6010";
inline const char *NetworkRequestFailed      = "DC-6011";

// ---- 语音 DC-7xxx --------------------------------------------------------
inline const char *SpeechUnavailable      = "DC-7001";
inline const char *SpeechNoBackend        = "DC-7002";
inline const char *SpeechDeviceError      = "DC-7003";
inline const char *SpeechRecognitionFailed= "DC-7004";
inline const char *SpeechLowConfidence    = "DC-7005";
inline const char *SpeechSynthesisFailed  = "DC-7006";

// ---- 录音 DC-8xxx --------------------------------------------------------
inline const char *RecordingDisabled    = "DC-8001";
inline const char *RecordingNotStarted  = "DC-8002";
inline const char *RecordingLimitTime   = "DC-8003";
inline const char *RecordingLimitSpace  = "DC-8004";
inline const char *RecordingDeviceError = "DC-8005";
inline const char *RecordingWriteFailed = "DC-8006";

// ---- 历史记录 DC-9xxx ----------------------------------------------------
inline const char *HistoryDisabled      = "DC-9001";
inline const char *HistoryDbOpenFailed  = "DC-9002";
inline const char *HistoryQueryFailed   = "DC-9003";
inline const char *HistoryDeleteFailed  = "DC-9004";
inline const char *HistoryExportFailed  = "DC-9005";
inline const char *HistoryDbCorrupted   = "DC-9006";

// ---- 平台 DC-Axxx --------------------------------------------------------
inline const char *PlatformUnsupported       = "DC-A001";
inline const char *PlatformCapabilityMissing = "DC-A002";
inline const char *PlatformHotkeyConflict    = "DC-A003";
inline const char *PlatformHotkeyRegisterFail= "DC-A004";
inline const char *PlatformAutostartFailed   = "DC-A005";

// ---------------------------------------------------------------------------
// 错误码 -> 面向用户的中文说明
//   返回空字符串表示该错误码没有预设文案，调用方应使用传入的自定义消息。
// ---------------------------------------------------------------------------
QString describe(const QString &errorCode);

// 错误码分类前缀，便于统计与界面分组
QString categoryOf(const QString &errorCode);

} // namespace erc
} // namespace dc
