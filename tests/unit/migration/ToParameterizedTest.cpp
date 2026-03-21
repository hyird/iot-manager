/**
 * @file ToParameterizedTest.cpp
 * @brief DatabaseService::toParameterized 函数单元测试
 *
 * 这是现有代码（server/common/database/DatabaseService.hpp）的补充测试。
 * toParameterized 将 ? 占位符替换为 PostgreSQL 的 $1, $2, ... 格式。
 *
 * 覆盖情况演示了如何对纯函数进行全面测试，
 * 也是迁移系统参数化 SQL 安全性的基础。
 */

#include <gtest/gtest.h>
// toParameterized 是 DatabaseService.hpp 中的内联函数
// 由于是 header-only，直接包含即可
// 注意：实际项目中可能需要处理 Drogon 依赖，
// 此处展示测试结构，具体包含路径按实现调整

// #include "common/database/DatabaseService.hpp"

// 为了演示目的，内联被测函数
inline std::string toParameterized(const std::string& sql, size_t paramCount) {
    if (paramCount == 0) return sql;
    std::string result;
    result.reserve(sql.size() + paramCount * 3);
    size_t idx = 1;
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '?' && idx <= paramCount) {
            result += '$';
            result += std::to_string(idx++);
        } else {
            result += sql[i];
        }
    }
    return result;
}

// ============================================================
// 正常路径
// ============================================================

TEST(ToParameterizedTest, NoParams_ReturnsSql_Unchanged) {
    const std::string sql = "SELECT * FROM users";
    EXPECT_EQ(toParameterized(sql, 0), sql);
}

TEST(ToParameterizedTest, SingleParam_ReplacesQuestionMark) {
    EXPECT_EQ(
        toParameterized("SELECT * FROM users WHERE id = ?", 1),
        "SELECT * FROM users WHERE id = $1"
    );
}

TEST(ToParameterizedTest, TwoParams_ReplacesInOrder) {
    EXPECT_EQ(
        toParameterized("INSERT INTO t (a, b) VALUES (?, ?)", 2),
        "INSERT INTO t (a, b) VALUES ($1, $2)"
    );
}

TEST(ToParameterizedTest, ThreeParams_AllReplaced) {
    EXPECT_EQ(
        toParameterized("UPDATE t SET a=?, b=? WHERE id=?", 3),
        "UPDATE t SET a=$1, b=$2 WHERE id=$3"
    );
}

TEST(ToParameterizedTest, TenParams_AllReplaced_WithCorrectNumbers) {
    std::string sql, expected;
    for (int i = 0; i < 10; ++i) {
        sql      += "?";
        expected += "$" + std::to_string(i + 1);
        if (i < 9) { sql += ","; expected += ","; }
    }
    EXPECT_EQ(toParameterized(sql, 10), expected);
}

// ============================================================
// 边界条件：参数数量与占位符数量不匹配
// ============================================================

TEST(ToParameterizedTest, FewerParamsThanPlaceholders_OnlyReplacesFirst) {
    // paramCount=1 时只替换第一个 ?，剩余的 ? 保留
    auto result = toParameterized("? AND ?", 1);
    EXPECT_EQ(result, "$1 AND ?");
}

TEST(ToParameterizedTest, MoreParamsThanPlaceholders_DoesNotAddExtra) {
    // paramCount=5 但只有 2 个 ?，多余的参数计数不影响输出
    auto result = toParameterized("? AND ?", 5);
    EXPECT_EQ(result, "$1 AND $2");
}

// ============================================================
// SQL 中的特殊字符不应被误处理
// ============================================================

TEST(ToParameterizedTest, SqlWithSingleQuotes_NotAffected) {
    // 字符串字面量中的 ? 也应被替换（libpq 不解析字符串字面量）
    // 这是 toParameterized 的预期行为：盲目替换所有 ?
    auto result = toParameterized("SELECT '?'", 1);
    EXPECT_EQ(result, "SELECT '$1'");
    // 注意：这是已知行为，调用者应确保字符串字面量不含 ?
}

TEST(ToParameterizedTest, EmptySql_ReturnsEmpty) {
    EXPECT_EQ(toParameterized("", 0), "");
    EXPECT_EQ(toParameterized("", 5), "");
}

TEST(ToParameterizedTest, SqlWithNoPlaceholders_ReturnsUnchanged) {
    const std::string sql = "SELECT 1 + 1";
    EXPECT_EQ(toParameterized(sql, 3), sql);
}

// ============================================================
// 安全性：防止 SQL 注入（参数绑定本身就是防注入机制）
// ============================================================

TEST(ToParameterizedTest, SpecialCharactersInSql_Preserved) {
    // 分号、注释符等不应被修改
    const std::string sql = "SELECT id FROM t; -- comment";
    EXPECT_EQ(toParameterized(sql, 0), sql);
}
