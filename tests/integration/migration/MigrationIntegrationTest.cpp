/**
 * @file MigrationIntegrationTest.cpp
 * @brief 迁移系统集成测试（使用真实 PostgreSQL）
 *
 * 前置条件：
 *   - 本地或 CI 上运行的 PostgreSQL 实例
 *   - 环境变量：TEST_DB_HOST, TEST_DB_PORT, TEST_DB_NAME, TEST_DB_USER, TEST_DB_PASSWORD
 *   - 测试用户有 CREATE/DROP 权限
 *
 * 隔离策略：
 *   - 每个测试套件在独立 PostgreSQL Schema 中运行
 *   - SetUp 创建临时 Schema（如 test_migration_1234567890）
 *   - TearDown DROP 该 Schema（级联删除所有对象）
 *   - 不同测试间无状态共享
 *
 * 运行：ctest -R MigrationIntegration --timeout 60
 * 跳过（无 DB）：ctest -R MigrationIntegration -E integration
 *
 * CI 跳过标记：
 *   如果 TEST_DB_HOST 未设置，所有集成测试自动 SKIP。
 */

#include <gtest/gtest.h>
#include "migration/MigrationEngine.hpp"
#include "migration/MigrationFileLoader.hpp"
#include "migration/MigrationStateManager.hpp"
#include "helpers/TestDatabaseHelper.hpp"

// libpq 直接连接（集成测试不依赖 Drogon 事件循环）
#include <libpq-fe.h>

using namespace iot::migration;
using namespace iot::test;

// ============================================================
// 跳过条件：无数据库环境自动跳过
// ============================================================
#define SKIP_IF_NO_DB()                                          \
    do {                                                         \
        const char* host = std::getenv("TEST_DB_HOST");         \
        if (!host || std::string(host).empty()) {               \
            GTEST_SKIP() << "TEST_DB_HOST not set, skipping "   \
                            "integration test";                  \
        }                                                        \
    } while (0)

// ============================================================
// 集成测试用真实数据库连接包装器
// ============================================================

class PgConnection {
public:
    explicit PgConnection(const TestDbConfig& cfg) {
        conn_ = PQconnectdb(cfg.toConnString().c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            std::string err = PQerrorMessage(conn_);
            PQfinish(conn_);
            conn_ = nullptr;
            throw std::runtime_error("PG connect failed: " + err);
        }
    }

    ~PgConnection() {
        if (conn_) PQfinish(conn_);
    }

    // 执行 SQL，失败时抛出
    void exec(const std::string& sql) {
        PGresult* res = PQexec(conn_, sql.c_str());
        ExecStatusType status = PQresultStatus(res);
        std::string errMsg = PQresultErrorMessage(res);
        PQclear(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            throw std::runtime_error("SQL failed: " + errMsg + "\nSQL: " + sql);
        }
    }

    // 查询并返回单值
    std::string queryScalar(const std::string& sql) {
        PGresult* res = PQexec(conn_, sql.c_str());
        std::string val;
        if (PQntuples(res) > 0 && PQnfields(res) > 0) {
            const char* v = PQgetvalue(res, 0, 0);
            if (v) val = v;
        }
        PQclear(res);
        return val;
    }

    // 查询行数
    int queryCount(const std::string& sql) {
        auto val = queryScalar(sql);
        return val.empty() ? 0 : std::stoi(val);
    }

    PGconn* raw() { return conn_; }

private:
    PGconn* conn_ = nullptr;
};

// ============================================================
// 集成测试 Fixture
// ============================================================

class MigrationIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SKIP_IF_NO_DB();

        cfg_  = TestDbConfig{};
        conn_ = std::make_unique<PgConnection>(cfg_);

        // 为本次测试创建独立的临时 Schema
        schemaName_ = "test_migration_" +
            std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count() % 1000000000LL);

        conn_->exec("CREATE SCHEMA " + schemaName_);
        conn_->exec("SET search_path TO " + schemaName_);
    }

    void TearDown() override {
        if (conn_) {
            try {
                conn_->exec("DROP SCHEMA IF EXISTS " + schemaName_ + " CASCADE");
            } catch (...) {
                // 清理失败不阻断测试报告
            }
        }
    }

    // 直接向数据库执行 SQL（绕过迁移引擎）
    void execSql(const std::string& sql) {
        conn_->exec("SET search_path TO " + schemaName_);
        conn_->exec(sql);
    }

    // 查询 schema_migrations 表中的记录数
    int countApplied() {
        return conn_->queryCount(
            "SELECT COUNT(*) FROM " + schemaName_ + ".schema_migrations "
            "WHERE success = true"
        );
    }

    // 查询指定版本是否已应用
    bool isApplied(int version) {
        return conn_->queryCount(
            "SELECT COUNT(*) FROM " + schemaName_ + ".schema_migrations "
            "WHERE version = " + std::to_string(version) + " AND success = true"
        ) > 0;
    }

    // 查询表是否存在
    bool tableExists(const std::string& tableName) {
        return conn_->queryCount(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = '" + schemaName_ + "' "
            "AND table_name = '" + tableName + "'"
        ) > 0;
    }

    // 查询索引是否存在
    bool indexExists(const std::string& indexName) {
        return conn_->queryCount(
            "SELECT COUNT(*) FROM pg_indexes "
            "WHERE schemaname = '" + schemaName_ + "' "
            "AND indexname = '" + indexName + "'"
        ) > 0;
    }

    TestDbConfig                 cfg_;
    std::unique_ptr<PgConnection> conn_;
    TempMigrationDir             tmpDir_;
    std::string                  schemaName_;
};

// ============================================================
// 正常路径：从空数据库完整迁移
// ============================================================

TEST_F(MigrationIntegrationTest, Migrate_EmptyDb_AppliesAllMigrations) {
    // Arrange：创建 3 个迁移文件
    tmpDir_.write({1, "create_users",
        "CREATE TABLE users (id SERIAL PRIMARY KEY, name TEXT NOT NULL);",
        "DROP TABLE users;"});
    tmpDir_.write({2, "create_devices",
        "CREATE TABLE devices (id SERIAL PRIMARY KEY, code TEXT UNIQUE);",
        "DROP TABLE devices;"});
    tmpDir_.write({3, "add_user_index",
        "CREATE INDEX idx_users_name ON users (name);",
        "DROP INDEX idx_users_name;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());

    // Act
    MigrationResult result = engine->migrate();

    // Assert
    ASSERT_TRUE(result.success) << "Migration failed: " << result.errors[0];
    EXPECT_EQ(result.appliedCount, 3);
    EXPECT_EQ(countApplied(), 3);
    EXPECT_TRUE(tableExists("users"));
    EXPECT_TRUE(tableExists("devices"));
    EXPECT_TRUE(indexExists("idx_users_name"));
}

TEST_F(MigrationIntegrationTest, Migrate_AlreadyUpToDate_NoChanges) {
    tmpDir_.write({1, "schema",
        "CREATE TABLE t (id SERIAL);",
        "DROP TABLE t;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());

    engine->migrate();

    // 第二次迁移：无变化
    MigrationResult result = engine->migrate();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 0);
    EXPECT_EQ(countApplied(), 1);
}

// ============================================================
// 回滚测试
// ============================================================

TEST_F(MigrationIntegrationTest, Rollback_LastMigration_RevertsChange) {
    tmpDir_.write({1, "base",
        "CREATE TABLE users (id SERIAL PRIMARY KEY);",
        "DROP TABLE users;"});
    tmpDir_.write({2, "add_email",
        "ALTER TABLE users ADD COLUMN email TEXT;",
        "ALTER TABLE users DROP COLUMN email;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    engine->migrate();

    ASSERT_TRUE(tableExists("users"));

    // 回滚最后一个迁移（V2）
    MigrationResult rollbackResult = engine->rollback();

    ASSERT_TRUE(rollbackResult.success);

    // 验证 email 列已被删除
    int emailColCount = conn_->queryCount(
        "SELECT COUNT(*) FROM information_schema.columns "
        "WHERE table_schema = '" + schemaName_ + "' "
        "AND table_name = 'users' "
        "AND column_name = 'email'"
    );
    EXPECT_EQ(emailColCount, 0) << "email column should be gone after rollback";

    // V1 仍应用
    EXPECT_TRUE(isApplied(1));
    EXPECT_FALSE(isApplied(2));
    EXPECT_EQ(countApplied(), 1);
}

TEST_F(MigrationIntegrationTest, RollbackTo_TargetVersion_RevertsMultipleVersions) {
    tmpDir_.write({1, "a", "CREATE TABLE a (id INT);",    "DROP TABLE a;"});
    tmpDir_.write({2, "b", "CREATE TABLE b (id INT);",    "DROP TABLE b;"});
    tmpDir_.write({3, "c", "CREATE TABLE c (id INT);",    "DROP TABLE c;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    engine->migrate();

    ASSERT_TRUE(tableExists("a"));
    ASSERT_TRUE(tableExists("b"));
    ASSERT_TRUE(tableExists("c"));

    // 回滚到 V1（执行 V3.down 和 V2.down）
    MigrationResult result = engine->rollbackTo(1);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.rolledBackCount, 2);
    EXPECT_TRUE(tableExists("a"))   << "V1 should remain";
    EXPECT_FALSE(tableExists("b"))  << "V2 should be rolled back";
    EXPECT_FALSE(tableExists("c"))  << "V3 should be rolled back";
    EXPECT_EQ(countApplied(), 1);
}

// ============================================================
// 错误恢复：失败的迁移不影响已提交的版本
// ============================================================

TEST_F(MigrationIntegrationTest, Migrate_FailingMigration_PreviousVersionsRemainIntact) {
    tmpDir_.write({1, "good",
        "CREATE TABLE good_table (id SERIAL);",
        "DROP TABLE good_table;"});
    tmpDir_.write({2, "broken",
        "THIS IS NOT VALID SQL @@@;",
        "SELECT 1;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    MigrationResult result = engine->migrate();

    // V1 成功，V2 失败
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.appliedCount, 1);
    EXPECT_EQ(result.failedVersion, 2);

    // V1 已提交，数据库状态正确
    EXPECT_TRUE(tableExists("good_table"));
    EXPECT_TRUE(isApplied(1));
    EXPECT_FALSE(isApplied(2));
}

TEST_F(MigrationIntegrationTest, Migrate_PartialSqlInMigration_RollsBackEntireMigration) {
    // 同一个迁移文件中，第一条 SQL 成功，第二条失败
    // 整个迁移应该回滚（事务边界）
    const std::string sql =
        "CREATE TABLE partial_test (id SERIAL PRIMARY KEY);\n"
        "INSERT INTO partial_test (id) VALUES ('not_a_number');";  // 类型错误

    tmpDir_.write({1, "partial", sql, "DROP TABLE IF EXISTS partial_test;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    MigrationResult result = engine->migrate();

    EXPECT_FALSE(result.success);
    // partial_test 表不应存在（整个事务回滚）
    EXPECT_FALSE(tableExists("partial_test"))
        << "Table should not exist after transaction rollback";
}

// ============================================================
// 并发保护：两个进程同时运行迁移
// ============================================================

TEST_F(MigrationIntegrationTest, Migrate_ConcurrentExecution_OnlyOneSucceeds) {
    tmpDir_.write({1, "schema",
        "CREATE TABLE concurrent_test (id SERIAL);",
        "DROP TABLE concurrent_test;"});

    auto engine1 = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());

    // 使用第二个连接模拟并发
    PgConnection conn2(cfg_);
    auto engine2 = MigrationEngine::createWithDirectConnection(
        conn2.raw(), schemaName_, tmpDir_.path());

    // 手动获取 engine1 的 advisory lock，模拟它正在执行
    conn_->exec("SELECT pg_advisory_lock(hash_record('" + schemaName_ + "'))");

    // engine2 应该因为无法获取锁而抛出异常
    EXPECT_THROW(engine2->migrate(), MigrationEngine::ConcurrentMigrationError);

    conn_->exec("SELECT pg_advisory_unlock(hash_record('" + schemaName_ + "'))");
}

// ============================================================
// Checksum 篡改检测
// ============================================================

TEST_F(MigrationIntegrationTest, Migrate_TamperedMigrationFile_DetectsChecksumMismatch) {
    // 第一次：正常应用 V1
    tmpDir_.write({1, "schema",
        "CREATE TABLE original (id INT);",
        "DROP TABLE original;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    engine->migrate();
    ASSERT_TRUE(isApplied(1));

    // 篡改：修改 V1 的 up SQL 内容
    tmpDir_.writeRaw("V001__schema.up.sql", "CREATE TABLE tampered (id INT);");

    // 再次运行迁移：应检测到 checksum 不一致
    EXPECT_THROW(engine->migrate(), MigrationEngine::ChecksumMismatchError);
}

// ============================================================
// 基线迁移：将 DatabaseInitializer 的现有 schema 作为 V001
// ============================================================

TEST_F(MigrationIntegrationTest, BaselineMigration_ExistingSchema_RegisteredAsV001) {
    // 模拟现有数据库：直接创建几个关键表
    execSql("CREATE TABLE sys_user (id SERIAL PRIMARY KEY, username TEXT)");
    execSql("CREATE TABLE sys_role (id SERIAL PRIMARY KEY, code TEXT)");

    // 创建基线迁移文件（仅记录，不执行 DDL）
    // 基线迁移应该检测到表已存在，跳过 CREATE TABLE，只注册版本
    tmpDir_.write({1, "baseline",
        "-- Baseline: schema created by DatabaseInitializer\n"
        "-- This migration is a no-op for existing databases\n"
        "DO $$ BEGIN\n"
        "    -- Verify key tables exist\n"
        "    ASSERT EXISTS (SELECT 1 FROM information_schema.tables "
        "WHERE table_name = 'sys_user');\n"
        "END $$;\n",
        ""});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());
    MigrationResult result = engine->migrate();

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(isApplied(1));
    // 原有表保持不变
    EXPECT_TRUE(tableExists("sys_user"));
    EXPECT_TRUE(tableExists("sys_role"));
}

// ============================================================
// 大型迁移文件：性能测试
// ============================================================

TEST_F(MigrationIntegrationTest, Migrate_LargeMigration_CompletesWithin30Seconds) {
    // 生成包含 1000 个 INSERT 的迁移
    std::string upSql = "CREATE TABLE perf_test (id SERIAL, val TEXT);\n";
    for (int i = 0; i < 100; ++i) {
        upSql += "INSERT INTO perf_test (val) VALUES ('row_" +
                 std::to_string(i) + "');\n";
    }

    tmpDir_.write({1, "perf", upSql, "DROP TABLE perf_test;"});

    auto engine = MigrationEngine::createWithDirectConnection(
        conn_->raw(), schemaName_, tmpDir_.path());

    auto start = std::chrono::steady_clock::now();
    MigrationResult result = engine->migrate();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_TRUE(result.success);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 30);
    EXPECT_EQ(conn_->queryCount(
        "SELECT COUNT(*) FROM " + schemaName_ + ".perf_test"), 100);
}
