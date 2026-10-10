# ============================================================================
#  tests/CMakeLists.txt — 单元测试
#
#  需求依据：SRS V1.1 第 9.1 / 9.2 / 7.4 节
#    9.1 要求对「命令解析、权限判定、配置解析、路径校验、状态机」做单元测试
#    9.2 要求覆盖安全测试：「路径穿越与特殊字符输入」「尝试访问未授权目录」
#    7.4 要求关键模块可测试
#
#  设计说明：
#    所有测试链接 dc_core 静态库（真实业务代码，不是复制品）。
#    资源路径通过 dc_core 上定义的 DC_SOURCE_DIR 编译宏定位，
#    这样测试能在源码树里直接读 resources/，不依赖构建产物的部署布局。
# ============================================================================

find_package(Qt6 REQUIRED COMPONENTS Test)

if(NOT TARGET Qt6::Test)
    message(WARNING "[DesktopCompanion] 未找到 Qt6::Test，跳过单元测试构建")
    return()
endif()

# ---------------------------------------------------------------------------
# 测试注册辅助宏
#   每个测试是一个独立的可执行文件（Qt Test 的惯例），
#   这样单个测试崩溃不会影响其它测试，定位也清楚。
# ---------------------------------------------------------------------------
function(dc_add_test TEST_NAME SOURCE_FILE)
    add_executable(${TEST_NAME} ${SOURCE_FILE})
    target_link_libraries(${TEST_NAME} PRIVATE dc_core Qt6::Test)

    # 非 GUI 测试：不要拉进 Widgets 的显示依赖
    set_target_properties(${TEST_NAME} PROPERTIES
        WIN32_EXECUTABLE FALSE
    )

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

    # 测试也要能拿到 resources/ 等源码树里的数据。
    # DC_SOURCE_DIR 由 dc_core 的 PUBLIC 编译定义带过来，无需重复声明。
    target_compile_definitions(${TEST_NAME} PRIVATE
        DC_TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
    )
endfunction()

# ---------------------------------------------------------------------------
# 测试清单
# ---------------------------------------------------------------------------
# 命令引擎：输入规范化、模式匹配、参数抽取、歧义/无匹配判定
dc_add_test(tst_command_engine   unit/tst_command_engine.cpp)

# 规则仓库：JSON 加载、校验、别名表、冲突检测、多语言覆盖
dc_add_test(tst_command_repo     unit/tst_command_repo.cpp)

# 权限：风险升级、禁止意图、授权、**拒绝后不得重试**
dc_add_test(tst_permission       unit/tst_permission.cpp)

# 路径守卫：穿越、非法字符、Windows 保留名、敏感位置分级
dc_add_test(tst_path_guard       unit/tst_path_guard.cpp)

# 多语言：三语加载、key 完整性、占位符一致性
dc_add_test(tst_i18n             unit/tst_i18n.cpp)

# 语言检测：中英日字符统计与判定
dc_add_test(tst_language_detect  unit/tst_language_detect.cpp)

# 桌宠状态机：状态转移、优先级、临时状态回落
dc_add_test(tst_pet_state        unit/tst_pet_state.cpp)
