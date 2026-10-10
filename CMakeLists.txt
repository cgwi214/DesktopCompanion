# ============================================================================
#  Desktop Companion - 桌面宠物助手
#  CMake 构建入口
#
#  需求依据：SRS V1.1 第 8.4 节（构建与发布）
#  设计要点：
#    1. 按操作系统 / 架构区分构建配置
#    2. 固定并记录 Qt、编译器、第三方库版本（见 docs/BUILD_AND_COMPAT.md）
#    3. 平台适配代码通过条件编译隔离，业务代码不散布 OS 判断
#    4. 不捆绑未经许可的模型或组件（本项目零第三方模型依赖）
# ============================================================================

cmake_minimum_required(VERSION 3.16)

project(DesktopCompanion
    VERSION 1.0.0
    DESCRIPTION "轻量级桌面宠物助手（规则驱动 / 本地优先 / 不使用生成式 AI）"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 编译警告：把可疑写法暴露出来，便于安全审查
if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 选项开关（兼容性分级：核心功能与可选功能分离）
#     - 关闭某个可选功能后，核心桌宠与本地规则命令仍必须可用（SRS 6.2）
# ---------------------------------------------------------------------------
option(DC_ENABLE_SPEECH   "启用语音识别与合成模块（可选功能）" ON)
option(DC_ENABLE_NETWORK  "启用搜索/翻译/网络权限模块（可选功能）" ON)
option(DC_ENABLE_RECORDING "启用录音模块（可选功能）" ON)
option(DC_BUILD_TESTS     "构建单元测试" ON)
option(DC_ENABLE_LEGACY_GUARD "启用旧系统能力降级保护（SRS 6.1）" ON)

# ---------------------------------------------------------------------------
# Qt 依赖发现
#   需要模块：Core / Gui / Widgets / Sql / Network
#   Multimedia 仅用于录音与语音（可选），缺失时自动关闭对应功能而不是构建失败
#   —— 这是「能力探测控制功能入口」在构建期的体现（SRS 6.2）
# ---------------------------------------------------------------------------
set(DC_QT_REQUIRED_COMPONENTS Core Gui Widgets Sql Network)

find_package(Qt6 6.2 REQUIRED COMPONENTS ${DC_QT_REQUIRED_COMPONENTS})

set(DC_HAS_QT_MULTIMEDIA OFF)
if(DC_ENABLE_SPEECH OR DC_ENABLE_RECORDING)
    find_package(Qt6 QUIET COMPONENTS Multimedia)
    if(Qt6Multimedia_FOUND)
        set(DC_HAS_QT_MULTIMEDIA ON)
        message(STATUS "[DesktopCompanion] Qt Multimedia 已找到 -> 录音/音频采集功能启用")
    else()
        set(DC_ENABLE_SPEECH OFF)
        set(DC_ENABLE_RECORDING OFF)
        message(WARNING
            "[DesktopCompanion] 未找到 Qt Multimedia，语音与录音功能将在编译期禁用。"
            " 核心桌宠与本地命令功能不受影响（SRS 6.2）。")
    endif()
endif()

qt_standard_project_setup()

message(STATUS "[DesktopCompanion] Qt 版本      : ${Qt6_VERSION}")
message(STATUS "[DesktopCompanion] 编译器       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "[DesktopCompanion] 构建类型     : ${CMAKE_BUILD_TYPE}")
message(STATUS "[DesktopCompanion] 语音模块     : ${DC_ENABLE_SPEECH}")
message(STATUS "[DesktopCompanion] 录音模块     : ${DC_ENABLE_RECORDING}")
message(STATUS "[DesktopCompanion] 网络模块     : ${DC_ENABLE_NETWORK}")

# ---------------------------------------------------------------------------
# 生成编译期能力配置文件
#   把 CMake 的探测结果写进头文件，供 C++ 侧做条件编译与运行时能力声明
#
#   输出路径说明：源码里统一写作 #include "core/BuildConfig.h"，
#   因此必须放到 generated/core/ 下，这样 generated/ 作为包含目录时
#   该路径才成立。改这里就要同步改所有引用点 —— 所以别改。
# ---------------------------------------------------------------------------
string(TIMESTAMP DC_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)

file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated/core)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/BuildConfig.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/core/BuildConfig.h
    @ONLY
)

# ---------------------------------------------------------------------------
# sources.cmake —— 源文件清单集中管理
# ---------------------------------------------------------------------------
include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

# ---------------------------------------------------------------------------
# 共享静态库：core_lib
#   把全部业务逻辑放在库里，主程序和测试程序都链接它，
#   从而可以用 Qt Test 对真实代码（而非复制品）做单元测试。
# ---------------------------------------------------------------------------
add_library(dc_core STATIC ${DC_CORE_SOURCES} ${DC_CORE_HEADERS})

target_include_directories(dc_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(dc_core
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Sql
        Qt6::Network
)

if(DC_HAS_QT_MULTIMEDIA)
    target_link_libraries(dc_core PUBLIC Qt6::Multimedia)
endif()

# ---------------------------------------------------------------------------
# Windows 平台库
# ---------------------------------------------------------------------------
# 这两个是 MinGW 下"能编译但链接不过"的典型：
#   * crypt32 —— SecureStore 用 CryptProtectData / CryptUnprotectData 走 DPAPI
#                 加密保存 API Key。声明在 <wincrypt.h> 里，但实现要从 crypt32 导入，
#                 否则报 undefined reference to `__imp_CryptProtectData'。
#   * ole32 / oleaut32 / uuid —— SAPI 走 COM。CoInitialize / CoCreateInstance 由
#                 ole32 提供，VARIANT / SysAllocString 系列由 oleaut32 提供，
#                 普通 IID 由 uuid 提供。
#   * sapi —— CLSID_SpVoice / IID_ISpVoice / CLSID_SpObjectTokenCategory /
#                 IID_ISpRecognizer 这几个 SAPI 专属 GUID **不在** libuuid.a 里，
#                 MinGW 单独打包成了 libsapi.a（头文件 sapi51.h / sapi53.h /
#                 sapi54.h 里用 DEFINE_GUID 声明，所以编译期查不出来，只有链接
#                 期才报 undefined reference）。用 -lsapi 而不是绝对路径，让链接器
#                 走标准搜索路径，便于换 MinGW 版本。
# 只在 Windows 上加，保持 Linux 侧可编译（SRS 7.x 的跨平台目标）。
if(WIN32)
    target_link_libraries(dc_core PUBLIC
        crypt32
        ole32
        oleaut32
        uuid
        sapi
    )
endif()

target_compile_definitions(dc_core PUBLIC
    DC_APP_VERSION="${PROJECT_VERSION}"
    DC_ENABLE_SPEECH=$<BOOL:${DC_ENABLE_SPEECH}>
    DC_ENABLE_RECORDING=$<BOOL:${DC_ENABLE_RECORDING}>
    DC_ENABLE_NETWORK=$<BOOL:${DC_ENABLE_NETWORK}>
    DC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    $<$<BOOL:${DC_ENABLE_LEGACY_GUARD}>:DC_ENABLE_LEGACY_GUARD=1>
)

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
add_executable(DesktopCompanion WIN32 src/main.cpp)

target_link_libraries(DesktopCompanion PRIVATE dc_core)

set_target_properties(DesktopCompanion PROPERTIES
    OUTPUT_NAME "DesktopCompanion"
    WIN32_EXECUTABLE TRUE
)

# ---------------------------------------------------------------------------
# 单元测试（SRS 9.1 / 9.2 / 7.4）
# ---------------------------------------------------------------------------
if(DC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# 安装规则 / 便携包（SRS 8.4）
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS DesktopCompanion
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 配置与资源按目录安装，用户在便携模式下可直接编辑（SRS 8.2：JSON/INI 保存配置）
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/resources/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/DesktopCompanion/resources
    PATTERN "*.gitkeep" EXCLUDE
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/config/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/DesktopCompanion/config
)

message(STATUS "[DesktopCompanion] 配置完成。构建命令：cmake --build . --parallel")
