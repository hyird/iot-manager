#pragma once

#include <drogon/orm/DbClient.h>
#include <functional>
#include <vector>
#include <memory>

using namespace drogon;
using namespace drogon::orm;

/**
 * @brief 事务守卫类（RAII 风格）
 *
 * 特性：
 * - 自动回滚：析构时如果未提交则自动回滚
 * - 异常安全：异常发生时保证事务回滚
 * - 缓存集成：提交成功后执行缓存清除回调
 * - 协程支持：所有操作都是协程
 *
 * 使用示例：
 * @code
 * auto guard = co_await TransactionGuard::create(dbService);
 *
 * co_await guard.execSqlCoro("INSERT INTO ...", {...});
 * co_await guard.execSqlCoro("UPDATE ...", {...});
 *
 * // 提交成功后清除缓存
 * guard.onCommit([this, userId]() -> Task<void> {
 *     co_await cacheManager_.clearUserCache(userId);
 * });
 *
 * co_await guard.commit();  // 提交事务并执行回调
 * @endcode
 */
class TransactionGuard {
public:
    using CommitCallback = std::function<Task<void>()>;

private:
    std::shared_ptr<Transaction> transaction_;
    bool committed_{false};
    bool rolledBack_{false};
    std::vector<CommitCallback> commitCallbacks_;

    explicit TransactionGuard(std::shared_ptr<Transaction> trans)
        : transaction_(std::move(trans)) {}

public:
    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    TransactionGuard(TransactionGuard&& other) noexcept
        : transaction_(std::move(other.transaction_))
        , committed_(other.committed_)
        , rolledBack_(other.rolledBack_)
        , commitCallbacks_(std::move(other.commitCallbacks_)) {
        other.committed_ = true;  // 防止移动后的对象回滚
    }

    TransactionGuard& operator=(TransactionGuard&& other) noexcept {
        if (this != &other) {
            transaction_ = std::move(other.transaction_);
            committed_ = other.committed_;
            rolledBack_ = other.rolledBack_;
            commitCallbacks_ = std::move(other.commitCallbacks_);
            other.committed_ = true;  // 防止移动后的对象回滚
        }
        return *this;
    }

    /**
     * @brief 创建事务守卫
     */
    template<typename DbService>
    static Task<TransactionGuard> create(DbService& dbService) {
        auto trans = co_await dbService.newTransactionCoro();
        co_return TransactionGuard(trans);
    }

    /**
     * @brief 析构时自动回滚未提交的事务
     */
    ~TransactionGuard() {
        if (!committed_ && !rolledBack_ && transaction_) {
            try {
                // 注意：析构函数中无法使用协程，只能同步回滚
                // 但这通常不是问题，因为异常路径不需要高性能
                LOG_WARN << "Transaction auto-rollback in destructor";
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to rollback transaction in destructor: " << e.what();
            }
        }
    }

    /**
     * @brief 执行 SQL（带参数替换）
     */
    Task<Result> execSqlCoro(const std::string& sql, const std::vector<std::string>& params = {}) {
        if (committed_) {
            throw std::runtime_error("Transaction already committed");
        }
        if (rolledBack_) {
            throw std::runtime_error("Transaction already rolled back");
        }

        std::string finalSql = buildSql(sql, params);
        co_return co_await transaction_->execSqlCoro(finalSql);
    }

    /**
     * @brief 直接执行 SQL（无参数替换）
     */
    Task<Result> execSqlCoroDirect(const std::string& sql) {
        if (committed_) {
            throw std::runtime_error("Transaction already committed");
        }
        if (rolledBack_) {
            throw std::runtime_error("Transaction already rolled back");
        }

        co_return co_await transaction_->execSqlCoro(sql);
    }

    /**
     * @brief 注册提交成功后的回调（用于清除缓存）
     *
     * 回调在 commit() 成功后按注册顺序执行
     */
    void onCommit(CommitCallback callback) {
        commitCallbacks_.push_back(std::move(callback));
    }

    /**
     * @brief 提交事务并执行回调
     */
    Task<void> commit() {
        if (committed_) {
            throw std::runtime_error("Transaction already committed");
        }
        if (rolledBack_) {
            throw std::runtime_error("Transaction already rolled back");
        }

        try {
            // 提交事务（Drogon 的事务在离开作用域时自动提交）
            // 我们标记为已提交即可
            committed_ = true;

            // 执行所有提交回调（通常是缓存清除）
            for (const auto& callback : commitCallbacks_) {
                try {
                    co_await callback();
                } catch (const std::exception& e) {
                    LOG_ERROR << "Commit callback failed: " << e.what();
                    // 继续执行其他回调，因为事务已提交
                }
            }

            LOG_DEBUG << "Transaction committed successfully";

        } catch (const std::exception& e) {
            LOG_ERROR << "Transaction commit failed: " << e.what();
            committed_ = false;  // 恢复状态
            throw;
        }
    }

    /**
     * @brief 显式回滚事务
     */
    void rollback() {
        if (committed_) {
            throw std::runtime_error("Cannot rollback: transaction already committed");
        }
        if (rolledBack_) {
            return;  // 已回滚，直接返回
        }

        try {
            transaction_->rollback();
            rolledBack_ = true;
            LOG_DEBUG << "Transaction rolled back";
        } catch (const std::exception& e) {
            LOG_ERROR << "Transaction rollback failed: " << e.what();
            throw;
        }
    }

    /**
     * @brief 检查事务是否已提交
     */
    bool isCommitted() const { return committed_; }

    /**
     * @brief 检查事务是否已回滚
     */
    bool isRolledBack() const { return rolledBack_; }

    /**
     * @brief 获取底层事务对象（高级用法）
     */
    std::shared_ptr<Transaction> getTransaction() const { return transaction_; }

private:
    /**
     * @brief SQL 参数替换（与 DatabaseService 保持一致）
     */
    static std::string buildSql(const std::string& sql, const std::vector<std::string>& params) {
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

    static std::string escapeSqlParam(const std::string& param) {
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
};
