#pragma once

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
    using Transaction = drogon::orm::Transaction;
    using Result = drogon::orm::Result;
    template<typename T = void> using Task = drogon::Task<T>;
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
                LOG_WARN << "Transaction auto-rollback in destructor";
                transaction_->rollback();
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to rollback transaction in destructor: " << e.what();
            }
        }
    }

    /**
     * @brief 执行 SQL（带参数绑定）
     * 使用 PostgreSQL 原生 $N 参数化查询，由 libpq 服务端绑定防止 SQL 注入
     */
    Task<Result> execSqlCoro(const std::string& sql, const std::vector<std::string>& params = {}) {
        if (committed_) {
            throw std::runtime_error("Transaction already committed");
        }
        if (rolledBack_) {
            throw std::runtime_error("Transaction already rolled back");
        }

        if (params.empty()) {
            co_return co_await transaction_->execSqlCoro(sql);
        }
        auto binder = *transaction_ << toParameterized(sql, params.size());
        for (const auto& p : params) {
            binder << p;
        }
        co_return co_await drogon::orm::internal::SqlAwaiter(std::move(binder));
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
     * @brief 提交事务并等待确认，然后执行回调
     *
     * 通过 setCommitCallback 挂起协程，等待 PostgreSQL 确认 COMMIT 后才继续，
     * 确保数据库写入真正完成后，再执行回调和后续的领域事件发布。
     */
    Task<void> commit() {
        if (committed_) {
            throw std::runtime_error("Transaction already committed");
        }
        if (rolledBack_) {
            throw std::runtime_error("Transaction already rolled back");
        }

        // 等待 PostgreSQL COMMIT 确认：
        //   1. 注册 setCommitCallback 作为恢复点
        //   2. tx_.reset() 析构 Transaction → 发送 COMMIT 命令
        //   3. 协程挂起，直到 PostgreSQL 响应到来触发回调
        //   4. 回调中 handle.resume() 恢复协程，保证 DB 写入已完成
        struct CommitAwaiter : drogon::CallbackAwaiter<bool> {
            std::shared_ptr<Transaction> tx_;
            explicit CommitAwaiter(std::shared_ptr<Transaction> tx)
                : tx_(std::move(tx)) {}

            void await_suspend(std::coroutine_handle<> handle) {
                tx_->setCommitCallback([this, handle](bool success) {
                    setValue(success);
                    handle.resume();
                });
                tx_.reset();  // 析构 → 发送 COMMIT 命令
            }
        };

        bool success = co_await CommitAwaiter{std::move(transaction_)};
        committed_ = true;

        if (!success) {
            throw std::runtime_error("Transaction commit failed");
        }

        // 执行所有提交回调（通常是缓存清除），在 COMMIT 确认后执行
        for (const auto& callback : commitCallbacks_) {
            try {
                co_await callback();
            } catch (const std::exception& e) {
                LOG_ERROR << "Commit callback failed: " << e.what();
            }
        }

        LOG_DEBUG << "Transaction committed successfully";
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

};
