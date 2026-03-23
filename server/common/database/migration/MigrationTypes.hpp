#pragma once

#include <string>
#include <vector>
#include <memory>
#include <stdexcept>
#include <drogon/drogon.h>

/**
 * @brief 迁移元信息
 */
struct MigrationInfo {
    int version;              // 版本号（递增整数）
    std::string name;         // 迁移名称（如 "Baseline"）
    std::string description;  // 迁移描述
    bool transactional;       // 是否在事务中执行（TimescaleDB DDL 不支持事务）
};

/**
 * @brief 迁移记录（对应 schema_migrations 表的行）
 */
struct MigrationRecord {
    int version;
    std::string name;
    std::string checksum;
    std::string executedAt;
    int executionTimeMs;
    bool success;
};

/**
 * @brief 迁移方向
 */
enum class MigrationDirection { Up, Down };

/**
 * @brief 迁移基类
 *
 * 所有版本化迁移必须继承此类，实现 up() 和 down() 方法。
 */
class MigrationBase {
public:
    using DbClientPtr = drogon::orm::DbClientPtr;
    using TransactionPtr = std::shared_ptr<drogon::orm::Transaction>;
    template<typename T = void> using Task = drogon::Task<T>;

    virtual ~MigrationBase() = default;

    /// 返回迁移元信息
    virtual MigrationInfo info() const = 0;

    /// 正向迁移（在事务中执行，除非 info().transactional == false）
    virtual Task<> up(const TransactionPtr& txn) = 0;

    /// 回滚迁移（在事务中执行）
    virtual Task<> down(const TransactionPtr& txn) = 0;

    /// 正向迁移（非事务模式，直接使用 DbClient）
    virtual Task<> upNoTxn([[maybe_unused]] const DbClientPtr& db) {
        // 默认抛出异常，非事务迁移必须覆盖此方法
        throw std::runtime_error(
            "Migration V" + std::to_string(info().version) +
            " marked as non-transactional but upNoTxn() not implemented");
        co_return;
    }

    /// 回滚迁移（非事务模式）
    virtual Task<> downNoTxn([[maybe_unused]] const DbClientPtr& db) {
        throw std::runtime_error(
            "Migration V" + std::to_string(info().version) +
            " non-transactional rollback not supported");
        co_return;
    }
};

/**
 * @brief 迁移执行结果
 */
struct MigrationResult {
    int applied = 0;
    int skipped = 0;
    int failed = 0;
    int currentVersion = 0;
    std::vector<std::string> errors;

    bool ok() const { return failed == 0 && errors.empty(); }
};

/**
 * @brief 迁移异常
 */
class MigrationException : public std::runtime_error {
public:
    int version;
    explicit MigrationException(int ver, const std::string& msg)
        : std::runtime_error("Migration V" + std::to_string(ver) + ": " + msg)
        , version(ver) {}
};

/**
 * @brief Checksum 不匹配异常
 */
class ChecksumMismatchException : public MigrationException {
public:
    std::string expected;
    std::string actual;
    ChecksumMismatchException(int ver, const std::string& exp, const std::string& act)
        : MigrationException(ver,
            "checksum mismatch (recorded: " + exp + ", current: " + act + ")")
        , expected(exp), actual(act) {}
};
