#pragma once

#include "MigrationRegistry.hpp"
#include "../DatabaseService.hpp"
#include <chrono>
#include <cstdio>

/**
 * @brief 数据库迁移执行引擎
 *
 * 核心职责：
 * - 创建/管理 schema_migrations 版本追踪表
 * - 检测已有数据库并自动标记基线
 * - 按版本顺序执行待应用的迁移
 * - 在事务中执行每个迁移，失败自动回滚
 * - 使用 PostgreSQL Advisory Lock 防止并发迁移
 * - 支持回滚到指定版本
 */
// 将版本号格式化为 3 位补零字符串，如 1 -> "001"
static inline std::string fmtVersion(int v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%03d", v);
    return buf;
}

class MigrationRunner {
public:
    using DbClientPtr = drogon::orm::DbClientPtr;
    using TransactionPtr = std::shared_ptr<drogon::orm::Transaction>;
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 执行所有待应用的迁移（主入口）
     */
    static Task<MigrationResult> migrate(
        const DbClientPtr& db,
        MigrationRegistry& registry
    ) {
        MigrationResult result;

        // 校验迁移注册表
        registry.validate();

        // 获取分布式锁
        bool locked = co_await acquireLock(db);
        if (!locked) {
            result.failed = 1;
            result.errors.push_back(
                "Another migration is in progress. "
                "If this is incorrect, run: "
                "SELECT pg_advisory_unlock(hashtext('iot_schema_migration'))");
            co_return result;
        }

        try {
            // 确保 schema_migrations 表存在
            co_await ensureMigrationTable(db);

            // 基线检测：已有数据库自动标记基线版本
            co_await autoBaseline(db, registry);

            // 获取当前版本
            int currentVersion = co_await getCurrentVersion(db);
            result.currentVersion = currentVersion;

            // 执行待应用的迁移
            for (auto& migration : registry.getMigrations()) {
                auto migInfo = migration->info();

                if (migInfo.version <= currentVersion) {
                    result.skipped++;
                    continue;
                }

                try {
                    co_await executeMigration(db, *migration, MigrationDirection::Up);
                    result.applied++;
                    result.currentVersion = migInfo.version;

                    LOG_INFO << "[Migration] Applied V"
                             << fmtVersion(migInfo.version)
                             << " - " << migInfo.name;
                } catch (const std::exception& e) {
                    result.failed++;
                    result.errors.push_back(
                        "V" + std::to_string(migInfo.version) + " " +
                        migInfo.name + ": " + e.what());
                    LOG_ERROR << "[Migration] Failed V" << migInfo.version
                              << " - " << migInfo.name << ": " << e.what();
                    break;  // 遇到错误停止后续迁移
                }
            }

        } catch (const std::exception& e) {
            result.failed++;
            result.errors.push_back(std::string("Migration system error: ") + e.what());
        }
        co_await releaseLock(db);

        co_return result;
    }

    /**
     * @brief 回滚到指定版本
     *
     * 按版本号降序执行 down()，直到当前版本 == targetVersion。
     * 注意：回滚是危险操作，仅建议在开发环境使用。
     */
    static Task<MigrationResult> rollback(
        const DbClientPtr& db,
        MigrationRegistry& registry,
        int targetVersion
    ) {
        MigrationResult result;

        bool locked = co_await acquireLock(db);
        if (!locked) {
            result.failed = 1;
            result.errors.push_back("Another migration is in progress.");
            co_return result;
        }

        try {
            int currentVersion = co_await getCurrentVersion(db);
            result.currentVersion = currentVersion;

            if (targetVersion >= currentVersion) {
                LOG_INFO << "[Migration] Already at version " << currentVersion
                         << ", target " << targetVersion << ", nothing to rollback";
                co_await releaseLock(db);
                co_return result;
            }

            // 从高版本到低版本逐个回滚
            auto& migrations = registry.getMigrations();
            for (auto it = migrations.rbegin(); it != migrations.rend(); ++it) {
                auto migInfo = (*it)->info();

                if (migInfo.version <= targetVersion) break;
                if (migInfo.version > currentVersion) continue;

                try {
                    // 回滚：down() + 删除迁移记录在同一事务中
                    co_await executeRollback(db, **it);
                    result.applied++;
                    result.currentVersion = migInfo.version - 1;

                    LOG_INFO << "[Migration] Rolled back V" << migInfo.version
                             << " - " << migInfo.name;
                } catch (const std::exception& e) {
                    result.failed++;
                    result.errors.push_back(
                        "Rollback V" + std::to_string(migInfo.version) + ": " + e.what());
                    LOG_ERROR << "[Migration] Rollback failed V" << migInfo.version
                              << ": " << e.what();
                    break;
                }
            }
        } catch (const std::exception& e) {
            result.failed++;
            result.errors.push_back(std::string("Rollback error: ") + e.what());
        }
        co_await releaseLock(db);

        co_return result;
    }

    /**
     * @brief 获取当前数据库版本号
     */
    static Task<int> getCurrentVersion(const DbClientPtr& db) {
        try {
            auto r = co_await db->execSqlCoro(
                "SELECT COALESCE(MAX(version), 0) AS ver "
                "FROM schema_migrations WHERE success = true");
            if (!r.empty()) {
                co_return r[0]["ver"].as<int>();
            }
        } catch (const std::exception& e) {
            // schema_migrations 表可能还未创建
            LOG_DEBUG << "[Migration] getCurrentVersion failed (table may not exist): " << e.what();
        }
        co_return 0;
    }

    /**
     * @brief 获取所有迁移记录
     */
    static Task<std::vector<MigrationRecord>> getHistory(const DbClientPtr& db) {
        std::vector<MigrationRecord> records;
        try {
            auto rows = co_await db->execSqlCoro(
                "SELECT version, name, checksum, "
                "to_char(executed_at, 'YYYY-MM-DD HH24:MI:SS') as executed_at, "
                "execution_time_ms, success "
                "FROM schema_migrations ORDER BY version");
            for (const auto& row : rows) {
                records.push_back({
                    row["version"].as<int>(),
                    row["name"].as<std::string>(),
                    row["checksum"].as<std::string>(),
                    row["executed_at"].as<std::string>(),
                    row["execution_time_ms"].as<int>(),
                    row["success"].as<bool>()
                });
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[Migration] getHistory failed: " << e.what();
        }
        co_return records;
    }

private:
    // ─── Advisory Lock ──────────────────────────────────────

    static constexpr const char* LOCK_KEY = "iot_schema_migration";

    static Task<bool> acquireLock(const DbClientPtr& db) {
        try {
            auto r = co_await db->execSqlCoro(
                "SELECT pg_try_advisory_lock(hashtext($1)) AS acquired",
                LOCK_KEY);
            co_return !r.empty() && r[0]["acquired"].as<bool>();
        } catch (const std::exception& e) {
            LOG_ERROR << "[Migration] Failed to acquire advisory lock: " << e.what();
            co_return false;
        }
    }

    static Task<> releaseLock(const DbClientPtr& db) {
        try {
            co_await db->execSqlCoro(
                "SELECT pg_advisory_unlock(hashtext($1))",
                LOCK_KEY);
        } catch (const std::exception& e) {
            LOG_ERROR << "[Migration] Failed to release advisory lock: " << e.what()
                      << ". Lock will auto-release when session ends.";
        }
    }

    // ─── Migration Table ────────────────────────────────────

    static Task<> ensureMigrationTable(const DbClientPtr& db) {
        co_await db->execSqlCoro(R"(
            CREATE TABLE IF NOT EXISTS schema_migrations (
                version         INT NOT NULL PRIMARY KEY,
                name            VARCHAR(255) NOT NULL,
                checksum        VARCHAR(64) NOT NULL DEFAULT '',
                executed_at     TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
                execution_time_ms INT NOT NULL DEFAULT 0,
                success         BOOLEAN NOT NULL DEFAULT TRUE
            )
        )");
    }

    // ─── Baseline Detection ─────────────────────────────────

    /**
     * @brief 自动基线检测
     *
     * 条件：schema_migrations 为空 且 核心业务表已存在于 public schema
     * 行为：标记所有已存在的迁移版本为已执行（不实际执行 SQL）
     */
    static Task<> autoBaseline(
        const DbClientPtr& db,
        const MigrationRegistry& registry
    ) {
        // 已有迁移记录，跳过
        auto countResult = co_await db->execSqlCoro(
            "SELECT COUNT(*) AS c FROM schema_migrations");
        if (countResult[0]["c"].as<int64_t>() > 0) {
            co_return;
        }

        // 检测核心业务表是否存在于 public schema（三表同时存在 = 已有数据库）
        auto tableCheck = co_await db->execSqlCoro(R"(
            SELECT
                (EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'sys_user'))
                AND
                (EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'device'))
                AND
                (EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = 'public' AND table_name = 'link'))
            AS is_existing_db
        )");

        bool isExisting = tableCheck[0]["is_existing_db"].as<bool>();

        if (!isExisting) {
            LOG_INFO << "[Migration] Fresh database detected, will execute all migrations";
            co_return;
        }

        // 已有数据库：标记所有已注册的迁移为已执行
        LOG_INFO << "[Migration] Existing database detected, applying baseline markers...";

        for (const auto& migration : registry.getMigrations()) {
            auto migInfo = migration->info();
            co_await db->execSqlCoro(
                "INSERT INTO schema_migrations (version, name, checksum, execution_time_ms, success) "
                "VALUES ($1, $2, 'baseline', 0, true) ON CONFLICT DO NOTHING",
                migInfo.version,
                std::string("Baseline (auto-detected): " + migInfo.name));
        }

        LOG_INFO << "[Migration] All " << registry.size()
                 << " migrations marked as baseline (existing database)";
    }

    // ─── Migration Execution ────────────────────────────────

    /**
     * @brief 执行单个迁移（up 方向）
     */
    static Task<> executeMigration(
        const DbClientPtr& db,
        MigrationBase& migration,
        MigrationDirection direction
    ) {
        auto migInfo = migration.info();
        auto start = std::chrono::steady_clock::now();

        if (migInfo.transactional) {
            // 事务模式
            auto txn = co_await db->newTransactionCoro();
            try {
                if (direction == MigrationDirection::Up) {
                    co_await migration.up(txn);
                } else {
                    co_await migration.down(txn);
                }

                auto elapsed = getElapsedMs(start);

                // 记录迁移结果（仅 up 方向，在同一事务中）
                if (direction == MigrationDirection::Up) {
                    co_await txn->execSqlCoro(
                        "INSERT INTO schema_migrations "
                        "(version, name, checksum, execution_time_ms, success) "
                        "VALUES ($1, $2, $3, $4, true)",
                        migInfo.version,
                        migInfo.name,
                        std::string(""),  // checksum 预留
                        elapsed);
                }

                // Drogon Transaction 在正常析构时自动 COMMIT
            } catch (const std::exception& e) {
                // 事务会在 txn 析构时自动 ROLLBACK
                throw MigrationException(migInfo.version,
                    std::string(direction == MigrationDirection::Up ? "up" : "down") +
                    " failed: " + e.what());
            }
        } else {
            // 非事务模式（TimescaleDB DDL 等）
            // MSVC 不允许 catch 块内 co_await，用标志变量处理
            bool noTxnFailed = false;
            std::string noTxnError;
            try {
                if (direction == MigrationDirection::Up) {
                    co_await migration.upNoTxn(db);
                } else {
                    co_await migration.downNoTxn(db);
                }

                auto elapsed = getElapsedMs(start);

                if (direction == MigrationDirection::Up) {
                    co_await db->execSqlCoro(
                        "INSERT INTO schema_migrations "
                        "(version, name, checksum, execution_time_ms, success) "
                        "VALUES ($1, $2, $3, $4, true)",
                        migInfo.version,
                        migInfo.name,
                        std::string(""),
                        elapsed);
                }
            } catch (const std::exception& e) {
                noTxnFailed = true;
                noTxnError = e.what();
            }

            if (noTxnFailed) {
                // 记录失败（在 catch 块外执行 co_await）
                try {
                    co_await db->execSqlCoro(
                        "INSERT INTO schema_migrations "
                        "(version, name, checksum, execution_time_ms, success) "
                        "VALUES ($1, $2, $3, 0, false)",
                        migInfo.version,
                        migInfo.name,
                        std::string(""));
                } catch (const std::exception& recordErr) {
                    LOG_ERROR << "[Migration] Failed to record migration failure: " << recordErr.what();
                }

                throw MigrationException(migInfo.version,
                    std::string("non-transactional ") +
                    (direction == MigrationDirection::Up ? "up" : "down") +
                    " failed: " + noTxnError);
            }
        }
    }

    /**
     * @brief 执行回滚（down + 删除记录在同一事务中）
     */
    static Task<> executeRollback(
        const DbClientPtr& db,
        MigrationBase& migration
    ) {
        auto migInfo = migration.info();

        if (migInfo.transactional) {
            auto txn = co_await db->newTransactionCoro();
            try {
                co_await migration.down(txn);

                // 在同一事务中删除迁移记录
                co_await txn->execSqlCoro(
                    "DELETE FROM schema_migrations WHERE version = $1",
                    migInfo.version);

                // Drogon Transaction 析构时自动 COMMIT
            } catch (const std::exception& e) {
                throw MigrationException(migInfo.version,
                    std::string("rollback failed: ") + e.what());
            }
        } else {
            // 非事务型迁移回滚
            try {
                co_await migration.downNoTxn(db);

                co_await db->execSqlCoro(
                    "DELETE FROM schema_migrations WHERE version = $1",
                    migInfo.version);
            } catch (const std::exception& e) {
                throw MigrationException(migInfo.version,
                    std::string("non-transactional rollback failed: ") + e.what());
            }
        }
    }

    static int getElapsedMs(const std::chrono::steady_clock::time_point& start) {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count());
    }
};
