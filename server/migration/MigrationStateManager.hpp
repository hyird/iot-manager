#pragma once

/**
 * @file MigrationStateManager.hpp
 * @brief 迁移状态管理器接口
 *
 * 负责：
 *   1. 创建并管理 schema_migrations 追踪表
 *   2. 查询已应用的迁移版本集合
 *   3. 插入/更新迁移执行记录
 *   4. Checksum 校验（防止迁移文件被篡改）
 *   5. PostgreSQL Advisory Lock（防止并发迁移）
 *
 * schema_migrations 表结构：
 *   CREATE TABLE schema_migrations (
 *       version      INT PRIMARY KEY,
 *       description  TEXT NOT NULL,
 *       checksum     CHAR(64) NOT NULL,      -- SHA-256 hex
 *       applied_at   TIMESTAMPTZ NOT NULL DEFAULT NOW(),
 *       success      BOOLEAN NOT NULL DEFAULT TRUE,
 *       error_msg    TEXT,
 *       execution_ms INT                      -- 执行耗时
 *   );
 *
 * Advisory Lock Key：hash_record('schema_migrations')
 * 保证同一数据库同一时刻只有一个迁移进程运行。
 */

#include "MigrationTypes.hpp"
#include <memory>
#include <set>
#include <vector>
#include <functional>

namespace iot::migration {

// 抽象数据库接口（单元测试用 Mock 替代，集成测试用真实连接）
class IDbConnection {
public:
    virtual ~IDbConnection() = default;
    virtual void execSql(const std::string& sql,
                         const std::vector<std::string>& params = {}) = 0;
    virtual std::vector<std::vector<std::string>> querySql(
        const std::string& sql,
        const std::vector<std::string>& params = {}) = 0;
};

class MigrationStateManager {
public:
    // 异常类型别名
    using ChecksumMismatchError     = ChecksumMismatchException;
    using MigrationNotAppliedError  = MigrationNotAppliedException;
    using ConcurrentMigrationError  = ConcurrentMigrationException;

    /**
     * @brief RAII Advisory Lock 守卫
     */
    class LockGuard {
    public:
        explicit LockGuard(MigrationStateManager* mgr) : mgr_(mgr) {}
        ~LockGuard() { if (mgr_) mgr_->releaseLock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        MigrationStateManager* mgr_;
    };

    /**
     * @brief 构造（接受任何实现了 MockDbClient 接口的 db 对象）
     *
     * 模板参数允许单元测试注入 MockDbClient，
     * 集成测试注入真实 PgConnection 包装器。
     */
    template<typename DbClient>
    explicit MigrationStateManager(std::shared_ptr<DbClient> db)
        : db_(std::make_shared<DbClientAdapter<DbClient>>(std::move(db))) {}

    // ============================================================
    // 表管理
    // ============================================================
    void ensureMigrationTable();

    // ============================================================
    // 版本查询
    // ============================================================
    std::set<int>    getAppliedVersions();
    std::vector<int> getPendingVersions(const std::vector<int>& available);

    // ============================================================
    // 记录写入
    // ============================================================
    void recordMigration(const MigrationRecord& rec);
    void removeMigrationRecord(int version);

    // ============================================================
    // Checksum 校验
    // ============================================================
    void verifyChecksum(int version, const std::string& currentChecksum);

    // ============================================================
    // Advisory Lock
    // ============================================================
    void      acquireLock();
    void      releaseLock();
    LockGuard acquireLockGuard();

private:
    // 类型擦除适配器（允许 MockDbClient 和真实连接两种形式）
    struct IDbAdapter {
        virtual ~IDbAdapter() = default;
        virtual void execSql(const std::string& sql,
                             const std::vector<std::string>& params) = 0;
        virtual std::vector<std::vector<std::string>> querySql(
            const std::string& sql,
            const std::vector<std::string>& params) = 0;
    };

    template<typename DbClient>
    struct DbClientAdapter : IDbAdapter {
        std::shared_ptr<DbClient> db;
        explicit DbClientAdapter(std::shared_ptr<DbClient> d) : db(std::move(d)) {}

        void execSql(const std::string& sql,
                     const std::vector<std::string>& params) override {
            db->execSql(sql, params);
        }
        std::vector<std::vector<std::string>> querySql(
            const std::string& sql,
            const std::vector<std::string>& params) override {
            return db->querySql(sql, params);
        }
    };

    std::shared_ptr<IDbAdapter> db_;

    static constexpr long long ADVISORY_LOCK_KEY = 0x6D69677261746530LL; // "migrate0"
};

} // namespace iot::migration
