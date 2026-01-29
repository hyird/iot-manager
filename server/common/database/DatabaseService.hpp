#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/HttpAppFramework.h>
#include <memory>
#include <regex>

using namespace drogon;
using namespace drogon::orm;

/**
 * @brief SQL 参数转义（防止 SQL 注入）
 */
inline std::string escapeSqlParam(const std::string& param) {
    std::string escaped;
    escaped.reserve(param.size() * 2);
    for (char c : param) {
        switch (c) {
            case '\'': escaped += "''"; break;
            case '\\': escaped += "\\\\"; break;
            case '\0': escaped += "\\0"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\x1a': escaped += "\\Z"; break;
            default: escaped += c;
        }
    }
    return escaped;
}

/**
 * @brief 替换 SQL 中的 ? 占位符
 */
inline std::string buildSql(const std::string& sql, const std::vector<std::string>& params) {
    if (params.empty()) return sql;

    std::string result;
    result.reserve(sql.size() + params.size() * 32);

    size_t paramIndex = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '?' && paramIndex < params.size()) {
            result += '\'';
            result += escapeSqlParam(params[paramIndex++]);
            result += '\'';
        } else {
            result += sql[i];
        }
    }
    return result;
}

/**
 * @brief 数据库配置（由 main.cpp 初始化）
 */
struct AppDbConfig {
    static bool& useFast() {
        static bool value = false;
        return value;
    }
};

/**
 * @brief 数据库服务类
 */
class DatabaseService {
public:
    DatabaseService() = default;

    DbClientPtr getClient() const {
        return AppDbConfig::useFast()
            ? app().getFastDbClient("default")
            : app().getDbClient("default");
    }

    Task<Result> execSqlCoro(const std::string& sql,
                              const std::vector<std::string>& params = {}) {
        std::string finalSql = buildSql(sql, params);
        co_return co_await getClient()->execSqlCoro(finalSql);
    }

    Task<std::shared_ptr<Transaction>> newTransactionCoro() {
        co_return co_await getClient()->newTransactionCoro();
    }
};
