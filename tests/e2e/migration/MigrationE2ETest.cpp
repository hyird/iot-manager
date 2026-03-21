/**
 * @file MigrationE2ETest.cpp
 * @brief 迁移系统端到端测试
 *
 * 测试完整的用户场景，模拟真实部署流程：
 *   1. 全新安装（空数据库 -> 完整 schema）
 *   2. 版本升级（V1 -> V2 -> V3）
 *   3. 回滚到指定版本
 *   4. 多次重启应用（幂等性）
 *   5. 从 DatabaseInitializer 迁移到迁移系统（基线场景）
 *
 * 这些测试直接使用真实 PostgreSQL，验证用户可观察的行为结果，
 * 不关注内部实现细节。
 *
 * 运行：ctest -R MigrationE2E --timeout 120
 */

#include <gtest/gtest.h>
#include "migration/MigrationEngine.hpp"
#include "helpers/TestDatabaseHelper.hpp"
#include <libpq-fe.h>

using namespace iot::migration;
using namespace iot::test;

#define SKIP_IF_NO_DB()                                          \
    do {                                                         \
        if (!std::getenv("TEST_DB_HOST")) {                     \
            GTEST_SKIP() << "No test database available";        \
        }                                                        \
    } while (0)

// ============================================================
// E2E Fixture：完整的 schema 隔离
// ============================================================

class MigrationE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        SKIP_IF_NO_DB();

        cfg_  = TestDbConfig{};
        conn_ = connectTo(cfg_);
        schemaName_ = "e2e_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count() % 1000000LL);

        execSql("CREATE SCHEMA " + schemaName_);
    }

    void TearDown() override {
        if (conn_) {
            try { execSql("DROP SCHEMA IF EXISTS " + schemaName_ + " CASCADE"); }
            catch (...) {}
            PQfinish(conn_);
        }
    }

    PGconn* connectTo(const TestDbConfig& cfg) {
        PGconn* c = PQconnectdb(cfg.toConnString().c_str());
        if (PQstatus(c) != CONNECTION_OK) {
            PQfinish(c);
            throw std::runtime_error("DB connect failed");
        }
        return c;
    }

    void execSql(const std::string& sql) {
        PGresult* r = PQexec(conn_, sql.c_str());
        ExecStatusType s = PQresultStatus(r);
        std::string err = PQresultErrorMessage(r);
        PQclear(r);
        if (s != PGRES_COMMAND_OK && s != PGRES_TUPLES_OK) {
            throw std::runtime_error("SQL: " + err);
        }
    }

    int queryCount(const std::string& sql) {
        PGresult* r = PQexec(conn_, sql.c_str());
        int count = 0;
        if (PQntuples(r) > 0) {
            const char* v = PQgetvalue(r, 0, 0);
            if (v) count = std::atoi(v);
        }
        PQclear(r);
        return count;
    }

    bool tableExists(const std::string& name) {
        return queryCount(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema='" + schemaName_ + "' AND table_name='" + name + "'"
        ) > 0;
    }

    bool columnExists(const std::string& table, const std::string& col) {
        return queryCount(
            "SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema='" + schemaName_ + "' "
            "AND table_name='" + table + "' AND column_name='" + col + "'"
        ) > 0;
    }

    std::unique_ptr<MigrationEngine> makeEngine(const std::string& migrationsPath) {
        return MigrationEngine::createWithDirectConnection(
            conn_, schemaName_, migrationsPath);
    }

    TestDbConfig  cfg_;
    PGconn*       conn_ = nullptr;
    TempMigrationDir tmpDir_;
    std::string   schemaName_;
};

// ============================================================
// 场景一：全新安装
// ============================================================

TEST_F(MigrationE2ETest, Scenario_FreshInstall_FullSchemaCreated) {
    // === 代表实际 IoT Manager schema 的精简版本 ===

    // V001：基础系统表
    tmpDir_.write({1, "base_system_tables", R"(
        CREATE TYPE status_enum AS ENUM ('enabled', 'disabled');

        CREATE TABLE sys_user (
            id SERIAL PRIMARY KEY,
            username VARCHAR(50) NOT NULL,
            password_hash VARCHAR(255) NOT NULL,
            status status_enum DEFAULT 'enabled',
            created_at TIMESTAMPTZ DEFAULT NOW(),
            deleted_at TIMESTAMPTZ NULL
        );
        CREATE UNIQUE INDEX idx_sys_user_username
            ON sys_user (username) WHERE deleted_at IS NULL;

        CREATE TABLE sys_role (
            id SERIAL PRIMARY KEY,
            name VARCHAR(50) NOT NULL,
            code VARCHAR(50) NOT NULL,
            status status_enum DEFAULT 'enabled',
            deleted_at TIMESTAMPTZ NULL
        );
        CREATE UNIQUE INDEX idx_sys_role_code
            ON sys_role (code) WHERE deleted_at IS NULL;
    )", R"(
        DROP TABLE IF EXISTS sys_role;
        DROP TABLE IF EXISTS sys_user;
        DROP TYPE IF EXISTS status_enum;
    )"});

    // V002：设备管理表
    tmpDir_.write({2, "device_tables", R"(
        CREATE TABLE device (
            id SERIAL PRIMARY KEY,
            code VARCHAR(64) NOT NULL,
            name VARCHAR(100) NOT NULL,
            status status_enum DEFAULT 'enabled',
            created_at TIMESTAMPTZ DEFAULT NOW(),
            deleted_at TIMESTAMPTZ NULL
        );
        CREATE UNIQUE INDEX idx_device_code
            ON device (code) WHERE deleted_at IS NULL;
    )", R"(
        DROP TABLE IF EXISTS device;
    )"});

    // V003：初始化管理员
    tmpDir_.write({3, "seed_admin", R"(
        INSERT INTO sys_role (name, code) VALUES ('超级管理员', 'super_admin')
        ON CONFLICT DO NOTHING;
    )", R"(
        DELETE FROM sys_role WHERE code = 'super_admin';
    )"});

    auto engine = makeEngine(tmpDir_.path());
    MigrationResult result = engine->migrate();

    // 验证整个 schema 正确创建
    ASSERT_TRUE(result.success)
        << "Full schema migration failed: " << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_EQ(result.appliedCount, 3);

    EXPECT_TRUE(tableExists("sys_user"));
    EXPECT_TRUE(tableExists("sys_role"));
    EXPECT_TRUE(tableExists("device"));

    // 验证管理员角色已创建
    int adminCount = queryCount(
        "SELECT COUNT(*) FROM " + schemaName_ + ".sys_role "
        "WHERE code = 'super_admin'"
    );
    EXPECT_EQ(adminCount, 1);
}

// ============================================================
// 场景二：版本升级（增量部署）
// ============================================================

TEST_F(MigrationE2ETest, Scenario_IncrementalUpgrade_V1ToV3) {
    // 模拟：应用已运行 V1，现在升级到 V3

    // 准备所有迁移文件
    tmpDir_.write({1, "initial",
        "CREATE TABLE users (id SERIAL PRIMARY KEY, name TEXT);",
        "DROP TABLE users;"});
    tmpDir_.write({2, "add_email",
        "ALTER TABLE users ADD COLUMN email TEXT;",
        "ALTER TABLE users DROP COLUMN email;"});
    tmpDir_.write({3, "add_phone",
        "ALTER TABLE users ADD COLUMN phone TEXT;",
        "ALTER TABLE users DROP COLUMN phone;"});

    auto engine = makeEngine(tmpDir_.path());

    // 第一次部署：只应用到 V1
    engine->migrateTo(1);
    ASSERT_TRUE(tableExists("users"));
    ASSERT_FALSE(columnExists("users", "email"));

    // 第二次部署：升级到 V3
    MigrationResult result = engine->migrate();

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 2);  // V2 和 V3

    EXPECT_TRUE(columnExists("users", "email"));
    EXPECT_TRUE(columnExists("users", "phone"));
}

// ============================================================
// 场景三：回滚热修复
// ============================================================

TEST_F(MigrationE2ETest, Scenario_HotfixRollback_RevertsProblematicMigration) {
    // 模拟生产事故：V3 引入了错误，需要回滚

    tmpDir_.write({1, "base",
        "CREATE TABLE products (id SERIAL PRIMARY KEY, name TEXT NOT NULL);",
        "DROP TABLE products;"});
    tmpDir_.write({2, "add_price",
        "ALTER TABLE products ADD COLUMN price DECIMAL(10,2) DEFAULT 0;",
        "ALTER TABLE products DROP COLUMN price;"});
    tmpDir_.write({3, "buggy_constraint",
        "ALTER TABLE products ADD CONSTRAINT chk_price CHECK (price > 1000000);",
        "ALTER TABLE products DROP CONSTRAINT chk_price;"});

    auto engine = makeEngine(tmpDir_.path());
    engine->migrate();

    ASSERT_TRUE(columnExists("products", "price"));

    // 发现 V3 的约束太严格，触发回滚
    MigrationResult rollbackResult = engine->rollback();

    ASSERT_TRUE(rollbackResult.success);

    // V3 的约束已被移除，V1 和 V2 保持
    EXPECT_TRUE(tableExists("products"));
    EXPECT_TRUE(columnExists("products", "price"));

    // 验证约束已被删除（可以插入超过 1000000 的价格）
    EXPECT_NO_THROW(execSql(
        "INSERT INTO " + schemaName_ + ".products (name, price) "
        "VALUES ('Test', 999.99)"
    ));
}

// ============================================================
// 场景四：应用多次重启（幂等性）
// ============================================================

TEST_F(MigrationE2ETest, Scenario_MultipleRestarts_Idempotent) {
    tmpDir_.write({1, "schema",
        "CREATE TABLE idempotent_test (id SERIAL PRIMARY KEY, val TEXT);",
        "DROP TABLE idempotent_test;"});
    tmpDir_.write({2, "seed",
        "INSERT INTO idempotent_test (val) VALUES ('initial');",
        "DELETE FROM idempotent_test WHERE val = 'initial';"});

    auto engine = makeEngine(tmpDir_.path());

    // 模拟 5 次应用重启
    for (int restart = 1; restart <= 5; ++restart) {
        MigrationResult result = engine->migrate();
        EXPECT_TRUE(result.success)
            << "Restart " << restart << " failed";
        EXPECT_EQ(result.appliedCount, 0)
            << "Should be no-op after first migration (restart " << restart << ")";
    }

    // 数据只有一条（seed 只执行了一次）
    int rowCount = queryCount(
        "SELECT COUNT(*) FROM " + schemaName_ + ".idempotent_test"
    );
    EXPECT_EQ(rowCount, 1);
}

// ============================================================
// 场景五：从 DatabaseInitializer 迁移（基线场景）
// ============================================================

TEST_F(MigrationE2ETest, Scenario_MigrateFromLegacyInitializer_RegistersBaseline) {
    // 模拟：现有系统已用 DatabaseInitializer 创建了 schema
    execSql("SET search_path TO " + schemaName_);
    execSql("CREATE TABLE sys_user (id SERIAL PRIMARY KEY, username TEXT UNIQUE NOT NULL)");
    execSql("CREATE TABLE sys_role (id SERIAL PRIMARY KEY, code TEXT UNIQUE NOT NULL)");
    execSql("CREATE TABLE device (id SERIAL PRIMARY KEY, code TEXT UNIQUE NOT NULL)");
    execSql("INSERT INTO sys_user (username) VALUES ('admin')");

    // 创建基线迁移（V001 = 现有 schema 的"快照标记"）
    tmpDir_.write({1, "baseline_existing_schema",
        "-- Baseline migration: existing schema registered as V001\n"
        "-- All tables were created by DatabaseInitializer\n"
        "-- This is a no-op for existing databases\n"
        "DO $$ BEGIN\n"
        "    ASSERT EXISTS ("
        "        SELECT 1 FROM information_schema.tables "
        "        WHERE table_name = 'sys_user'"
        "    ), 'sys_user table must exist for baseline';\n"
        "END $$;\n",
        ""  // 基线迁移无 down（不能删除生产数据）
    });

    // V002：新功能（迁移系统上线后的第一个增量变更）
    tmpDir_.write({2, "add_sys_menu",
        "CREATE TABLE IF NOT EXISTS sys_menu (\n"
        "    id SERIAL PRIMARY KEY,\n"
        "    name VARCHAR(50) NOT NULL,\n"
        "    path VARCHAR(255)\n"
        ");",
        "DROP TABLE IF EXISTS sys_menu;"
    });

    auto engine = makeEngine(tmpDir_.path());
    MigrationResult result = engine->migrate();

    ASSERT_TRUE(result.success)
        << "Baseline + incremental migration failed: "
        << (result.errors.empty() ? "" : result.errors[0]);
    EXPECT_EQ(result.appliedCount, 2);

    // 原有数据完好
    int adminCount = queryCount(
        "SELECT COUNT(*) FROM " + schemaName_ + ".sys_user WHERE username = 'admin'"
    );
    EXPECT_EQ(adminCount, 1) << "Existing admin user should be preserved";

    // 新表已创建
    EXPECT_TRUE(tableExists("sys_menu"));
}

// ============================================================
// 场景六：边界——版本跳号部署
// ============================================================

TEST_F(MigrationE2ETest, Scenario_VersionGap_WarnsButProceeds) {
    // V1, V3 存在，V2 缺失（可能由另一个开发分支导致）
    tmpDir_.write({1, "first",  "CREATE TABLE t1 (id INT);", "DROP TABLE t1;"});
    tmpDir_.write({3, "third",  "CREATE TABLE t3 (id INT);", "DROP TABLE t3;"});

    auto engine = makeEngine(tmpDir_.path());
    MigrationResult result = engine->migrate();

    // 默认行为：警告但继续（或可配置为报错）
    // 这里验证默认的"警告并继续"行为
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 2);
    EXPECT_FALSE(result.warnings.empty())
        << "Expected warning about version gap (missing V2)";
    EXPECT_TRUE(tableExists("t1"));
    EXPECT_TRUE(tableExists("t3"));
}

// ============================================================
// 场景七：网络中断恢复
// （通过断开连接后重新连接，验证部分应用后的恢复）
// ============================================================

TEST_F(MigrationE2ETest, Scenario_ReconnectAfterPartialMigration_ResumesCorrectly) {
    tmpDir_.write({1, "a", "CREATE TABLE recover_a (id SERIAL);", "DROP TABLE recover_a;"});
    tmpDir_.write({2, "b", "CREATE TABLE recover_b (id SERIAL);", "DROP TABLE recover_b;"});
    tmpDir_.write({3, "c", "CREATE TABLE recover_c (id SERIAL);", "DROP TABLE recover_c;"});

    {
        // 第一次连接：应用 V1 和 V2
        auto engine = makeEngine(tmpDir_.path());
        engine->migrateTo(2);
        ASSERT_TRUE(tableExists("recover_a"));
        ASSERT_TRUE(tableExists("recover_b"));
    }
    // 模拟连接断开（engine 析构）

    {
        // 第二次连接（模拟应用重启）：应该只应用 V3
        PGconn* newConn = PQconnectdb(cfg_.toConnString().c_str());
        ASSERT_EQ(PQstatus(newConn), CONNECTION_OK);
        PQexec(newConn, ("SET search_path TO " + schemaName_).c_str());

        auto engine2 = MigrationEngine::createWithDirectConnection(
            newConn, schemaName_, tmpDir_.path());
        MigrationResult result = engine2->migrate();

        EXPECT_TRUE(result.success);
        EXPECT_EQ(result.appliedCount, 1);  // 只应用了 V3
        EXPECT_TRUE(tableExists("recover_c"));

        PQfinish(newConn);
    }
}
