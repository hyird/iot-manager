#!/usr/bin/env bash
# =============================================================
# IoT Manager 迁移测试运行脚本
#
# 用法：
#   ./tests/run_tests.sh [unit|integration|e2e|all] [--coverage]
#
# 环境变量：
#   TEST_DB_HOST      PostgreSQL 主机（默认 localhost）
#   TEST_DB_PORT      端口（默认 5432）
#   TEST_DB_NAME      数据库名（默认 iot_manager_test）
#   TEST_DB_USER      用户名（默认 postgres）
#   TEST_DB_PASSWORD  密码（默认 postgres）
#
# CI 用法（GitHub Actions / GitLab CI）：
#   export TEST_DB_HOST=localhost
#   export TEST_DB_PASSWORD=ci_password
#   ./tests/run_tests.sh all
# =============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
TEST_BIN_DIR="${BUILD_DIR}/release/tests"

MODE="${1:-unit}"
ENABLE_COVERAGE="${2:-}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# =========================================================
# 检查测试二进制是否存在
# =========================================================
check_binary() {
    local bin="$1"
    if [[ ! -f "${TEST_BIN_DIR}/${bin}" ]]; then
        log_error "Binary not found: ${TEST_BIN_DIR}/${bin}"
        log_error "请先在 VSCode CMake Tools 中构建测试目标"
        exit 1
    fi
}

# =========================================================
# 运行单元测试（无数据库依赖）
# =========================================================
run_unit_tests() {
    log_info "Running unit tests..."
    check_binary "iot_migration_unit_tests"

    "${TEST_BIN_DIR}/iot_migration_unit_tests" \
        --gtest_output="xml:${BUILD_DIR}/test_results_unit.xml" \
        --gtest_color=yes \
        2>&1 | tee "${BUILD_DIR}/test_output_unit.log"

    local exit_code=${PIPESTATUS[0]}
    if [[ $exit_code -eq 0 ]]; then
        log_info "Unit tests PASSED"
    else
        log_error "Unit tests FAILED (exit code: $exit_code)"
        exit $exit_code
    fi
}

# =========================================================
# 运行集成测试（需要数据库）
# =========================================================
run_integration_tests() {
    log_info "Running integration tests..."

    if [[ -z "${TEST_DB_HOST:-}" ]]; then
        log_warn "TEST_DB_HOST not set, skipping integration tests"
        log_warn "Set TEST_DB_HOST=localhost to run integration tests"
        return 0
    fi

    check_binary "iot_migration_integration_tests"

    export TEST_DB_HOST="${TEST_DB_HOST:-localhost}"
    export TEST_DB_PORT="${TEST_DB_PORT:-5432}"
    export TEST_DB_NAME="${TEST_DB_NAME:-iot_manager_test}"
    export TEST_DB_USER="${TEST_DB_USER:-postgres}"
    export TEST_DB_PASSWORD="${TEST_DB_PASSWORD:-postgres}"

    "${TEST_BIN_DIR}/iot_migration_integration_tests" \
        --gtest_output="xml:${BUILD_DIR}/test_results_integration.xml" \
        --gtest_color=yes \
        2>&1 | tee "${BUILD_DIR}/test_output_integration.log"

    local exit_code=${PIPESTATUS[0]}
    if [[ $exit_code -eq 0 ]]; then
        log_info "Integration tests PASSED"
    else
        log_error "Integration tests FAILED (exit code: $exit_code)"
        exit $exit_code
    fi
}

# =========================================================
# 运行 E2E 测试
# =========================================================
run_e2e_tests() {
    log_info "Running E2E tests..."

    if [[ -z "${TEST_DB_HOST:-}" ]]; then
        log_warn "TEST_DB_HOST not set, skipping E2E tests"
        return 0
    fi

    check_binary "iot_migration_e2e_tests"

    "${TEST_BIN_DIR}/iot_migration_e2e_tests" \
        --gtest_output="xml:${BUILD_DIR}/test_results_e2e.xml" \
        --gtest_color=yes \
        2>&1 | tee "${BUILD_DIR}/test_output_e2e.log"

    local exit_code=${PIPESTATUS[0]}
    if [[ $exit_code -eq 0 ]]; then
        log_info "E2E tests PASSED"
    else
        log_error "E2E tests FAILED (exit code: $exit_code)"
        exit $exit_code
    fi
}

# =========================================================
# 覆盖率报告（仅 Linux/GCC）
# =========================================================
generate_coverage() {
    if [[ "$(uname)" != "Linux" ]]; then
        log_warn "Coverage report only supported on Linux with GCC"
        return 0
    fi

    if ! command -v lcov &> /dev/null; then
        log_warn "lcov not found, skipping coverage report"
        return 0
    fi

    log_info "Generating coverage report..."

    lcov --capture \
         --directory "${BUILD_DIR}" \
         --output-file "${BUILD_DIR}/coverage.info" \
         --quiet

    lcov --remove "${BUILD_DIR}/coverage.info" \
         '/usr/*' \
         '*/tests/*' \
         '*/vcpkg_installed/*' \
         --output-file "${BUILD_DIR}/coverage.info" \
         --quiet

    genhtml "${BUILD_DIR}/coverage.info" \
            --output-directory "${BUILD_DIR}/coverage_html" \
            --quiet

    # 检查覆盖率是否达到 80%
    local line_rate
    line_rate=$(lcov --summary "${BUILD_DIR}/coverage.info" 2>&1 |
                grep "lines" | grep -oP '\d+\.\d+(?=%)' | head -1)

    log_info "Line coverage: ${line_rate}%"

    if (( $(echo "$line_rate < 80.0" | bc -l) )); then
        log_error "Coverage ${line_rate}% is below 80% threshold!"
        exit 1
    fi

    log_info "Coverage report: ${BUILD_DIR}/coverage_html/index.html"
}

# =========================================================
# 主流程
# =========================================================
mkdir -p "${BUILD_DIR}"

case "${MODE}" in
    unit)
        run_unit_tests
        ;;
    integration)
        run_integration_tests
        ;;
    e2e)
        run_e2e_tests
        ;;
    all)
        run_unit_tests
        run_integration_tests
        run_e2e_tests
        if [[ "${ENABLE_COVERAGE}" == "--coverage" ]]; then
            generate_coverage
        fi
        ;;
    coverage)
        run_unit_tests
        generate_coverage
        ;;
    *)
        echo "Usage: $0 [unit|integration|e2e|all|coverage] [--coverage]"
        exit 1
        ;;
esac

log_info "Done."
