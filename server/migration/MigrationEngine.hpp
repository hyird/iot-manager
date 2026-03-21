#pragma once

/**
 * @file MigrationEngine.hpp
 * @brief 迁移执行引擎接口（门面类）
 *
 * 协调 MigrationFileLoader + MigrationStateManager，
 * 执行完整的迁移/回滚流程。
 *
 * 职责：
 *   1. 获取 Advisory Lock（防并发）
 *   2. 通过 FileLoader 扫描可用迁移
 *   3. 通过 StateManager 查询待执行迁移
 *   4. 对每个 pending 迁移：
 *      a. 开启事务
 *      b. 执行 up SQL
 *      c. 插入 schema_migrations 记录
 *      d. 提交事务
 *      e. 失败时回滚并记录错误，停止后续迁移
 *   5. 释放 Advisory Lock
 *
 * 事务粒度：每个迁移版本一个独立事务。
 * 失败语义：单个版本失败时，已提交的版本保持，未执行的版本不执行。
 */

#include "MigrationTypes.hpp"
#include "MigrationFileLoader.hpp"
#include "MigrationStateManager.hpp"
#include <memory>
#include <string>

// 前向声明：集成/E2E 测试使用的直连工厂
struct pg_conn;
using PGconn = pg_conn;

namespace iot::migration {

class MigrationEngine {
public:
    // 异常类型别名（测试代码直接使用 MigrationEngine::XxxError）
    using ConcurrentMigrationError = ConcurrentMigrationException;
    using ChecksumMismatchError    = ChecksumMismatchException;
    using NothingToRollbackError   = NothingToRollbackException;
    using NoDownMigrationError     = NoDownMigrationException;
    using VersionNotFoundError     = VersionNotFoundException;

    /**
     * @brief 构造（单元测试使用，注入 MockDbClient）
     *
     * @tparam DbClient  实现了 execSql()/querySql() 接口的类型
     */
    template<typename DbClient>
    MigrationEngine(std::shared_ptr<DbClient> db, const std::string& migrationsDir)
        : loader_(std::make_unique<MigrationFileLoader>(migrationsDir))
        , stateManager_(std::make_shared<MigrationStateManager>(db))
        , rawDb_(db)
    {}

    /**
     * @brief 工厂方法：集成/E2E 测试使用真实 libpq 连接
     *
     * @param conn           已连接的 PGconn*（调用者持有所有权）
     * @param schemaName     目标 Schema（用于隔离）
     * @param migrationsDir  迁移 SQL 文件目录
     */
    static std::unique_ptr<MigrationEngine> createWithDirectConnection(
        PGconn* conn,
        const std::string& schemaName,
        const std::string& migrationsDir);

    // ============================================================
    // 主要操作
    // ============================================================

    /**
     * @brief 应用所有待执行的迁移（up 到最新版本）
     *
     * @throws ConcurrentMigrationError  另一个迁移进程正在运行
     * @throws ChecksumMismatchError     已应用文件的 checksum 发生变化
     * @return MigrationResult           包含 success、appliedCount、errors
     */
    MigrationResult migrate();

    /**
     * @brief 应用迁移直到指定目标版本
     *
     * @throws VersionNotFoundError  目标版本在文件系统中不存在
     * @throws ConcurrentMigrationError
     */
    MigrationResult migrateTo(int targetVersion);

    /**
     * @brief 回滚最后一个已应用的迁移
     *
     * @throws NothingToRollbackError  无已应用的迁移
     * @throws NoDownMigrationError    目标版本无 down 文件
     */
    MigrationResult rollback();

    /**
     * @brief 回滚到指定版本（执行所有 > targetVersion 的 down 迁移）
     *
     * @throws VersionNotFoundError    目标版本不在已应用列表中
     * @throws NoDownMigrationError    某个版本无 down 文件
     */
    MigrationResult rollbackTo(int targetVersion);

    /**
     * @brief 获取当前已应用的最大版本号（-1 表示未应用任何迁移）
     */
    int currentVersion();

    /**
     * @brief 获取待应用迁移数量（不执行）
     */
    int pendingCount();

private:
    std::unique_ptr<MigrationFileLoader>   loader_;
    std::shared_ptr<MigrationStateManager> stateManager_;
    std::shared_ptr<void>                  rawDb_;  ///< 类型擦除持有 db 共享指针

    MigrationResult doMigrate(int targetVersion);
    MigrationResult doRollback(int targetVersion);
    void applySingleMigration(int version, const std::string& sql,
                               const std::string& checksum,
                               const std::string& description);
    void rollbackSingleMigration(int version, const std::string& downSql);
};

} // namespace iot::migration
