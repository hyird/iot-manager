cmake_minimum_required(VERSION 3.20)

# ============================================================
# IoT Manager 测试套件
# ============================================================
# 构建方式：
#   cmake -S . -B build -DBUILD_TESTS=ON
#   cmake --build build --target iot_migration_tests
#   ctest --test-dir build -R migration
#
# 跳过集成/E2E 测试（无数据库）：
#   ctest --test-dir build -R unit
# ============================================================

# GoogleTest：通过 vcpkg 提供
find_package(GTest CONFIG REQUIRED)

# ============================================================
# 公共测试库（辅助工具 + Mock）
# ============================================================
add_library(iot_test_helpers INTERFACE)
target_include_directories(iot_test_helpers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/server
)
target_link_libraries(iot_test_helpers INTERFACE
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
)

# ============================================================
# 迁移系统头文件路径
# （迁移引擎实现在 server/migration/ 下，待创建）
# ============================================================
set(MIGRATION_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/server/migration")

# ============================================================
# 单元测试可执行文件
# ============================================================
add_executable(iot_migration_unit_tests
    unit/migration/MigrationVersionParserTest.cpp
    unit/migration/MigrationFileLoaderTest.cpp
    unit/migration/MigrationStateManagerTest.cpp
    unit/migration/MigrationEngineTest.cpp
)

target_include_directories(iot_migration_unit_tests PRIVATE
    ${MIGRATION_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/server
)

target_link_libraries(iot_migration_unit_tests PRIVATE
    iot_test_helpers
    # 迁移引擎库（待实现后取消注释）
    # iot_migration_lib
)

# MSVC 设置
if(MSVC)
    target_compile_options(iot_migration_unit_tests PRIVATE
        /W4 /EHsc /bigobj
        /wd4068 /wd4996 /wd4244 /wd4267
    )
    set_target_properties(iot_migration_unit_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ============================================================
# 集成测试可执行文件（需要真实数据库）
# ============================================================
add_executable(iot_migration_integration_tests
    integration/migration/MigrationIntegrationTest.cpp
)

target_include_directories(iot_migration_integration_tests PRIVATE
    ${MIGRATION_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/server
)

target_link_libraries(iot_migration_integration_tests PRIVATE
    iot_test_helpers
    # libpq（直接连接，不依赖 Drogon 事件循环）
    PostgreSQL::PostgreSQL
    # iot_migration_lib
)

if(MSVC)
    target_compile_options(iot_migration_integration_tests PRIVATE
        /W4 /EHsc /bigobj /wd4068 /wd4996 /wd4244 /wd4267
    )
    set_target_properties(iot_migration_integration_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ============================================================
# E2E 测试可执行文件
# ============================================================
add_executable(iot_migration_e2e_tests
    e2e/migration/MigrationE2ETest.cpp
)

target_include_directories(iot_migration_e2e_tests PRIVATE
    ${MIGRATION_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/server
)

target_link_libraries(iot_migration_e2e_tests PRIVATE
    iot_test_helpers
    PostgreSQL::PostgreSQL
    # iot_migration_lib
)

if(MSVC)
    target_compile_options(iot_migration_e2e_tests PRIVATE
        /W4 /EHsc /bigobj /wd4068 /wd4996 /wd4244 /wd4267
    )
    set_target_properties(iot_migration_e2e_tests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ============================================================
# CTest 注册
# ============================================================
include(GoogleTest)

# 单元测试（无外部依赖，本地开发和 CI 均可运行）
gtest_discover_tests(iot_migration_unit_tests
    TEST_PREFIX "Unit/"
    PROPERTIES LABELS "unit"
    TIMEOUT 30
)

# 集成测试（需要 PostgreSQL，CI 中通过 Docker 提供）
gtest_discover_tests(iot_migration_integration_tests
    TEST_PREFIX "Integration/"
    PROPERTIES
        LABELS "integration"
        ENVIRONMENT "TEST_DB_HOST=$ENV{TEST_DB_HOST}"
    TIMEOUT 60
)

# E2E 测试
gtest_discover_tests(iot_migration_e2e_tests
    TEST_PREFIX "E2E/"
    PROPERTIES
        LABELS "e2e"
        ENVIRONMENT "TEST_DB_HOST=$ENV{TEST_DB_HOST}"
    TIMEOUT 120
)

# ============================================================
# 覆盖率报告（仅 GCC/Clang）
# ============================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND ENABLE_COVERAGE)
    target_compile_options(iot_migration_unit_tests PRIVATE --coverage -O0 -g)
    target_link_options(iot_migration_unit_tests    PRIVATE --coverage)

    # 目标：80%+ 覆盖率
    # 生成报告：
    #   lcov --capture --directory . --output-file coverage.info
    #   lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file coverage.info
    #   genhtml coverage.info --output-directory coverage_html
    add_custom_target(coverage_report
        COMMAND lcov --capture --directory ${CMAKE_BINARY_DIR}
                     --output-file coverage.info
        COMMAND lcov --remove coverage.info '/usr/*' '*/tests/*' '*/vcpkg_installed/*'
                     --output-file coverage.info
        COMMAND genhtml coverage.info --output-directory ${CMAKE_BINARY_DIR}/coverage_html
        COMMAND ${CMAKE_COMMAND} -E echo
            "Coverage report: ${CMAKE_BINARY_DIR}/coverage_html/index.html"
        DEPENDS iot_migration_unit_tests
        COMMENT "Generating coverage report..."
    )
endif()

# ============================================================
# 输出测试二进制到统一目录
# ============================================================
foreach(target
        iot_migration_unit_tests
        iot_migration_integration_tests
        iot_migration_e2e_tests)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY
            "${PROJECT_SOURCE_DIR}/build/$<LOWER_CASE:$<CONFIG>>/tests"
    )
endforeach()
