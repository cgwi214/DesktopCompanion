# ============================================================================
#  Desktop Companion — 源文件清单
#
#  为什么单独放一个文件：
#    1. 主程序、静态库、测试三方共用同一份清单，避免漏文件导致的链接错误
#    2. 新增模块时只改这里，CMakeLists.txt 保持稳定
#    3. 目录即架构（SRS 3.1 分层架构），清单按层分组
# ============================================================================

# ---------------------------------------------------------------------------
# src/model —— 纯数据模型层（无 Qt UI 依赖，可独立单测）
# ---------------------------------------------------------------------------
set(DC_MODEL_HEADERS
    src/model/Types.h
    src/model/CommandRule.h
    src/model/ParseResult.h
    src/model/PermissionTypes.h
    src/model/HistoryRecord.h
)

set(DC_MODEL_SOURCES
    src/model/Types.cpp
    src/model/CommandRule.cpp
    src/model/ParseResult.cpp
    src/model/PermissionTypes.cpp
    src/model/HistoryRecord.cpp
)

# ---------------------------------------------------------------------------
# src/core —— 核心层：生命周期、配置、日志、错误
# ---------------------------------------------------------------------------
set(DC_CORE_LAYER_HEADERS
    src/core/BuildConfig.h.in
    src/core/Logger.h
    src/core/ErrorCode.h
    src/core/ConfigManager.h
    src/core/CoreRuntime.h
)

set(DC_CORE_LAYER_SOURCES
    src/core/Logger.cpp
    src/core/ErrorCode.cpp
    src/core/ConfigManager.cpp
    src/core/CoreRuntime.cpp
)

# ---------------------------------------------------------------------------
# src/platform —— 平台适配层（唯一允许出现 OS 判断的地方，SRS 3.2）
# ---------------------------------------------------------------------------
set(DC_PLATFORM_HEADERS
    src/platform/PathGuard.h
    src/platform/PlatformAdapter.h
    src/platform/SystemInfo.h
    src/platform/AutostartManager.h
    src/platform/HotkeyManager.h
)

set(DC_PLATFORM_SOURCES
    src/platform/PathGuard.cpp
    src/platform/PlatformAdapter.cpp
    src/platform/PlatformAdapter_win.cpp
    src/platform/PlatformAdapter_linux.cpp
    src/platform/SystemInfo.cpp
    src/platform/AutostartManager.cpp
    src/platform/HotkeyManager.cpp
)

# ---------------------------------------------------------------------------
# src/engine —— 命令引擎与权限管理（SRS 4.4 / 4.6）
# ---------------------------------------------------------------------------
set(DC_ENGINE_HEADERS
    src/engine/LanguageDetector.h
    src/engine/CommandRepository.h
    src/engine/CommandEngine.h
    src/engine/PermissionManager.h
    src/engine/TaskScheduler.h
    src/engine/IntentRouter.h
)

set(DC_ENGINE_SOURCES
    src/engine/LanguageDetector.cpp
    src/engine/CommandRepository.cpp
    src/engine/CommandEngine.cpp
    src/engine/PermissionManager.cpp
    src/engine/TaskScheduler.cpp
    src/engine/IntentRouter.cpp
)

# ---------------------------------------------------------------------------
# src/services —— 服务层（SRS 4.3 / 4.7 / 4.8 / 4.9 / 4.10 / 4.11）
# ---------------------------------------------------------------------------
set(DC_SERVICES_HEADERS
    src/services/HistoryService.h
    src/services/SystemService.h
    src/services/SecureStore.h
    src/services/NetworkService.h
    src/services/SearchService.h
    src/services/TranslationService.h
    src/services/SpeechService.h
    src/services/SpeechBackend.h
    src/services/SapiSpeechBackend.h
    src/services/RecorderService.h
)

set(DC_SERVICES_SOURCES
    src/services/HistoryService.cpp
    src/services/SystemService.cpp
    src/services/SecureStore.cpp
    src/services/NetworkService.cpp
    src/services/SearchService.cpp
    src/services/TranslationService.cpp
    src/services/SpeechService.cpp
    src/services/SpeechBackend.cpp
    src/services/SapiSpeechBackend.cpp
    src/services/RecorderService.cpp
)

# ---------------------------------------------------------------------------
# src/ui —— UI 展示层（SRS 4.1 / 4.2）
#   严格约束：本层不得直接执行系统命令或访问网络（SRS 3.2）
# ---------------------------------------------------------------------------
set(DC_UI_HEADERS
    src/ui/PetWindow.h
    src/ui/PetRenderer.h
    src/ui/SpeechBubble.h
    src/ui/PetStateMachine.h
    src/ui/CommandInputBox.h
    src/ui/PermissionDialog.h
    src/ui/SettingsDialog.h
    src/ui/HistoryDialog.h
    src/ui/RecorderPanel.h
    src/ui/TrayController.h
    src/ui/I18n.h
)

set(DC_UI_SOURCES
    src/ui/PetWindow.cpp
    src/ui/PetRenderer.cpp
    src/ui/SpeechBubble.cpp
    src/ui/PetStateMachine.cpp
    src/ui/CommandInputBox.cpp
    src/ui/PermissionDialog.cpp
    src/ui/SettingsDialog.cpp
    src/ui/HistoryDialog.cpp
    src/ui/RecorderPanel.cpp
    src/ui/TrayController.cpp
    src/ui/I18n.cpp
)

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
set(DC_CORE_HEADERS
    ${DC_MODEL_HEADERS}
    ${DC_CORE_LAYER_HEADERS}
    ${DC_PLATFORM_HEADERS}
    ${DC_ENGINE_HEADERS}
    ${DC_SERVICES_HEADERS}
    ${DC_UI_HEADERS}
)

set(DC_CORE_SOURCES
    ${DC_MODEL_SOURCES}
    ${DC_CORE_LAYER_SOURCES}
    ${DC_PLATFORM_SOURCES}
    ${DC_ENGINE_SOURCES}
    ${DC_SERVICES_SOURCES}
    ${DC_UI_SOURCES}
)
