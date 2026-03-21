#pragma once

/**
 * @file MockDbClient.hpp
 * @brief GoogleMock 风格的数据库客户端 Mock
 *
 * 由于 Drogon 的 DbClient 高度依赖事件循环，
 * 单元测试中使用此 Mock 替代真实连接，
 * 让迁移引擎的纯逻辑（版本解析、状态机、排序）
 * 可以在无数据库环境下快速运行。
 *
 * Mock 设计原则：
 *   - 记录所有 execSql 调用（用于断言调用顺序）
 *   - 可配置特定 SQL 抛出异常（模拟 SQL 错误）
 *   - 可配置返回预设结果集（模拟查询返回）
 *   - 线程安全（支持并发测试）
 */

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <mutex>
#include <optional>
#include <regex>
#include <map>

namespace iot::test {

/**
 * @brief Mock 查询结果中的单行数据
 */
struct MockRow {
    std::map<std::string, std::string> columns;

    std::string operator[](const std::string& col) const {
        auto it = columns.find(col);
        if (it == columns.end()) {
            throw std::out_of_range("Column not found: " + col);
        }
        return it->second;
    }
};

/**
 * @brief Mock 查询结果集
 */
struct MockResult {
    std::vector<MockRow> rows;
    std::size_t size() const { return rows.size(); }
    bool empty() const { return rows.empty(); }
    const MockRow& operator[](std::size_t i) const { return rows[i]; }
};

/**
 * @brief SQL 调用记录（用于断言）
 */
struct SqlCallRecord {
    std::string sql;
    std::vector<std::string> params;
};

/**
 * @brief SQL 错误注入规则
 *
 * 当 SQL 匹配 pattern 时，Mock 会抛出指定异常，
 * 用于测试迁移引擎的错误处理路径。
 */
struct SqlErrorRule {
    std::string  pattern;       // 正则表达式，匹配 SQL 内容
    std::string  errorMessage;  // 注入的错误消息
    int          triggerCount;  // -1 = 始终触发，N = 触发 N 次后恢复正常
};

/**
 * @brief Mock 数据库客户端
 *
 * 同步接口版本，适用于不依赖 Drogon 事件循环的纯逻辑单元测试。
 * 集成测试使用真实 libpq 连接。
 */
class MockDbClient {
public:
    MockDbClient() = default;

    // ============================================================
    // 配置接口（在测试 SetUp 中调用）
    // ============================================================

    /**
     * @brief 设置特定 SQL 模式的预设返回结果
     *
     * 用法示例（模拟 schema_migrations 查询返回已应用的版本）：
     * @code
     * mock.setResult("SELECT.*schema_migrations", MockResult{{
     *     {{"version","1"}, {"description","initial_schema"}, {"success","t"}}
     * }});
     * @endcode
     */
    void setResult(const std::string& sqlPattern, MockResult result) {
        std::lock_guard lock(mutex_);
        resultRules_.push_back({sqlPattern, std::move(result)});
    }

    /**
     * @brief 注入 SQL 错误（用于测试错误处理）
     *
     * @param sqlPattern  正则，匹配时抛出异常
     * @param errorMsg    异常消息
     * @param times       触发次数（-1 = 无限次）
     */
    void injectError(const std::string& sqlPattern,
                     const std::string& errorMsg,
                     int times = -1) {
        std::lock_guard lock(mutex_);
        errorRules_.push_back({sqlPattern, errorMsg, times});
    }

    /**
     * @brief 清除所有配置（在测试 TearDown 中调用）
     */
    void reset() {
        std::lock_guard lock(mutex_);
        callLog_.clear();
        resultRules_.clear();
        errorRules_.clear();
    }

    // ============================================================
    // 模拟执行接口
    // ============================================================

    /**
     * @brief 执行 SQL（同步 Mock）
     * @throws std::runtime_error 当 SQL 匹配错误注入规则时
     */
    MockResult execSql(const std::string& sql,
                       const std::vector<std::string>& params = {}) {
        std::lock_guard lock(mutex_);

        // 记录调用
        callLog_.push_back({sql, params});

        // 检查错误注入规则
        for (auto& rule : errorRules_) {
            if (std::regex_search(sql, std::regex(rule.pattern))) {
                if (rule.triggerCount != 0) {
                    if (rule.triggerCount > 0) --rule.triggerCount;
                    throw std::runtime_error(rule.errorMessage);
                }
            }
        }

        // 返回预设结果
        for (const auto& rule : resultRules_) {
            if (std::regex_search(sql, std::regex(rule.pattern))) {
                return rule.result;
            }
        }

        // 默认返回空结果集
        return MockResult{};
    }

    // ============================================================
    // 断言辅助接口（在测试断言阶段调用）
    // ============================================================

    /**
     * @brief 返回所有 SQL 调用记录（按调用顺序）
     */
    std::vector<SqlCallRecord> callLog() const {
        std::lock_guard lock(mutex_);
        return callLog_;
    }

    /**
     * @brief 返回调用次数
     */
    std::size_t callCount() const {
        std::lock_guard lock(mutex_);
        return callLog_.size();
    }

    /**
     * @brief 检查是否有 SQL 调用匹配给定模式
     */
    bool wasCalled(const std::string& sqlPattern) const {
        std::lock_guard lock(mutex_);
        std::regex re(sqlPattern);
        for (const auto& rec : callLog_) {
            if (std::regex_search(rec.sql, re)) return true;
        }
        return false;
    }

    /**
     * @brief 返回第 N 次 SQL 调用（0-based）
     */
    SqlCallRecord nthCall(std::size_t n) const {
        std::lock_guard lock(mutex_);
        if (n >= callLog_.size()) {
            throw std::out_of_range("Call index out of range: " + std::to_string(n));
        }
        return callLog_[n];
    }

    /**
     * @brief 清空调用记录（不清除规则）
     */
    void clearCallLog() {
        std::lock_guard lock(mutex_);
        callLog_.clear();
    }

private:
    struct ResultRule {
        std::string pattern;
        MockResult  result;
    };

    mutable std::mutex       mutex_;
    std::vector<SqlCallRecord> callLog_;
    std::vector<ResultRule>    resultRules_;
    std::vector<SqlErrorRule>  errorRules_;
};

} // namespace iot::test
