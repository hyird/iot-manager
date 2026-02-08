#pragma once

/**
 * @brief 将 SQL 中的 ? 占位符转换为 PostgreSQL 原生 $1, $2, ... 格式
 *
 * 配合 Drogon ORM 的 SqlBinder 使用，由 libpq 的 PQexecParams 执行
 * 服务端参数绑定，彻底防止 SQL 注入（无需客户端转义）
 */
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
    using DbClientPtr = drogon::orm::DbClientPtr;
    using Result = drogon::orm::Result;
    using Transaction = drogon::orm::Transaction;
    template<typename T = void> using Task = drogon::Task<T>;

    DatabaseService() = default;

    DbClientPtr getClient() const {
        return AppDbConfig::useFast()
            ? drogon::app().getFastDbClient("default")
            : drogon::app().getDbClient("default");
    }

    Task<void> ping() {
        co_await getClient()->execSqlCoro("SELECT 1");
    }

    Task<Result> execSqlCoro(const std::string& sql,
                              const std::vector<std::string>& params = {}) {
        if (params.empty()) {
            co_return co_await getClient()->execSqlCoro(sql);
        }
        // 使用 PostgreSQL 原生参数绑定（$1, $2, ...），由 libpq 服务端处理
        auto binder = *getClient() << toParameterized(sql, params.size());
        for (const auto& p : params) {
            binder << p;
        }
        co_return co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
    }

    Task<std::shared_ptr<Transaction>> newTransactionCoro() {
        co_return co_await getClient()->newTransactionCoro();
    }
};
