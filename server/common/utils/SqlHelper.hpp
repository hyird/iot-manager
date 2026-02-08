#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief SQL 构建辅助工具
 *
 * 提供通用的 IN 子句构建和批量插入 VALUES 构建
 */
namespace SqlHelper {

/**
 * @brief 构建 IN 子句的值列表（如 "1,2,3"）
 * @param ids ID 列表
 * @return 逗号分隔的字符串，空列表返回空字符串
 */
template <typename T>
std::string buildInClause(const std::vector<T>& ids) {
    if (ids.empty()) return "";

    std::ostringstream oss;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    return oss.str();
}

/**
 * @brief 构建 IN 子句的参数化占位符和参数（如 "?, ?, ?" + ["1", "2", "3"]）
 * @param ids ID 列表
 * @return {占位符字符串, 参数列表}
 */
template <typename T>
std::pair<std::string, std::vector<std::string>> buildParameterizedIn(const std::vector<T>& ids) {
    std::string placeholders;
    std::vector<std::string> params;
    params.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) placeholders += ", ";
        placeholders += "?";
        params.push_back(std::to_string(ids[i]));
    }
    return {placeholders, params};
}

/**
 * @brief 构建批量插入的 VALUES 子句和参数
 *
 * 生成 "(?, ?), (?, ?), ..." 格式的 SQL 片段及对应参数列表，
 * 用于多对多关联表的批量插入（如 sys_user_role, sys_role_menu）
 *
 * @param leftId 左侧关联 ID（如 userId, roleId）
 * @param rightIds 右侧关联 ID 列表（如 roleIds, menuIds）
 * @return {VALUES SQL 片段, 参数列表}
 */
template <typename T>
std::pair<std::string, std::vector<std::string>> buildBatchInsertValues(
    int leftId, const std::vector<T>& rightIds) {
    std::string sql;
    std::vector<std::string> params;
    params.reserve(rightIds.size() * 2);

    for (size_t i = 0; i < rightIds.size(); ++i) {
        if (i > 0) sql += ", ";
        sql += "(?, ?)";
        params.push_back(std::to_string(leftId));
        params.push_back(std::to_string(rightIds[i]));
    }

    return {sql, params};
}

}  // namespace SqlHelper
