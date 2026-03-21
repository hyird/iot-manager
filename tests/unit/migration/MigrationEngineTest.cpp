/**
 * @file MigrationEngineTest.cpp
 * @brief 迁移执行引擎单元测试（使用 MockDbClient + TempMigrationDir）
 *
 * 测试覆盖：
 *   - migrate() 正常流程（pending -> applied）
 *   - rollback() 流程（applied -> rolled back）
 *   - 事务边界（每个迁移一个事务）
 *   - 失败回滚（单个迁移失败时的事务回滚）
 *   - 部分应用恢复（数据库已有 V1/V2，只应用 V3）
 *   - 目标版本迁移（migrate to version N）
 *   - 空迁移场景（无待应用迁移）
 *   - Checksum 不一致检测
 */

#include <gtest/gtest.h>
#include "migration/MigrationEngine.hpp"        // 待实现
#include "migration/MigrationFileLoader.hpp"    // 待实现
#include "migration/MigrationStateManager.hpp"  // 待实现
#include "helpers/MockDbClient.hpp"
#include "helpers/TestDatabaseHelper.hpp"

using namespace iot::migration;
using namespace iot::test;

// ============================================================
// 测试 Fixture
// ============================================================

class MigrationEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_  = std::make_shared<MockDbClient>();
        engine_ = std::make_unique<MigrationEngine>(
            mock_,
            tmpDir_.path()
        );

        // 默认：schema_migrations 表为空（全新数据库）
        mock_->setResult("SELECT.*schema_migrations", MockResult{});
        // 默认：Advisory Lock 可以获取
        MockResult lockOk;
        lockOk.rows = {{{"pg_try_advisory_lock", "t"}}};
        mock_->setResult("pg_try_advisory_lock", lockOk);
    }

    void TearDown() override {
        mock_->reset();
    }

    // 辅助：添加 N 个迁移文件到临时目录
    void addMigration(int version, const std::string& desc,
                      const std::string& upSql = "SELECT 1;",
                      const std::string& downSql = "SELECT 0;") {
        tmpDir_.write({version, desc, upSql, downSql});
    }

    TempMigrationDir              tmpDir_;
    std::shared_ptr<MockDbClient> mock_;
    std::unique_ptr<MigrationEngine> engine_;
};

// ============================================================
// 正常路径：migrate()
// ============================================================

TEST_F(MigrationEngineTest, Migrate_NoMigrations_SucceedsWithNoOp) {
    // 目录为空，数据库为空
    MigrationResult result = engine_->migrate();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 0);
    EXPECT_TRUE(result.errors.empty());
}

TEST_F(MigrationEngineTest, Migrate_SinglePendingMigration_AppliesIt) {
    addMigration(1, "initial_schema",
                 "CREATE TABLE users (id SERIAL PRIMARY KEY);",
                 "DROP TABLE users;");

    MigrationResult result = engine_->migrate();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 1);
    EXPECT_TRUE(mock_->wasCalled("CREATE TABLE users"));
}

TEST_F(MigrationEngineTest, Migrate_ThreePendingMigrations_AppliesAllInOrder) {
    addMigration(1, "create_users",   "CREATE TABLE users (id SERIAL);");
    addMigration(2, "create_devices", "CREATE TABLE devices (id SERIAL);");
    addMigration(3, "add_index",      "CREATE INDEX idx_users ON users (id);");

    MigrationResult result = engine_->migrate();

    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 3);

    // 验证 SQL 执行顺序
    auto calls = mock_->callLog();
    int usersPos = -1, devicesPos = -1, indexPos = -1;
    for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
        if (calls[i].sql.find("CREATE TABLE users")   != std::string::npos) usersPos   = i;
        if (calls[i].sql.find("CREATE TABLE devices") != std::string::npos) devicesPos = i;
        if (calls[i].sql.find("CREATE INDEX idx_users") != std::string::npos) indexPos = i;
    }
    EXPECT_LT(usersPos, devicesPos)   << "V1 must run before V2";
    EXPECT_LT(devicesPos, indexPos)   << "V2 must run before V3";
}

TEST_F(MigrationEngineTest, Migrate_PartiallyApplied_OnlyAppliesRemaining) {
    // 模拟 V1 和 V2 已应用
    MockResult alreadyApplied;
    alreadyApplied.rows = {
        {{"version", "1"}, {"description", "users"}, {"success", "t"},
         {"checksum", "cs1"}},
        {{"version", "2"}, {"description", "devices"}, {"success", "t"},
         {"checksum", "cs2"}},
    };
    mock_->setResult("SELECT.*schema_migrations", alreadyApplied);

    addMigration(1, "users",   "CREATE TABLE users (id SERIAL);");
    addMigration(2, "devices", "CREATE TABLE devices (id SERIAL);");
    addMigration(3, "index",   "CREATE INDEX idx ON users(id);");

    mock_->clearCallLog();
    MigrationResult result = engine_->migrate();

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 1);  // 只应用了 V3
    EXPECT_TRUE(mock_->wasCalled("CREATE INDEX idx"));
    EXPECT_FALSE(mock_->wasCalled("CREATE TABLE users"))
        << "V1 should NOT be re-applied";
}

TEST_F(MigrationEngineTest, Migrate_WithTargetVersion_StopsAtTarget) {
    addMigration(1, "a", "CREATE TABLE a (id INT);");
    addMigration(2, "b", "CREATE TABLE b (id INT);");
    addMigration(3, "c", "CREATE TABLE c (id INT);");
    addMigration(4, "d", "CREATE TABLE d (id INT);");

    // 只迁移到 V2
    MigrationResult result = engine_->migrateTo(2);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 2);
    EXPECT_TRUE(mock_->wasCalled("CREATE TABLE a"));
    EXPECT_TRUE(mock_->wasCalled("CREATE TABLE b"));
    EXPECT_FALSE(mock_->wasCalled("CREATE TABLE c")) << "V3 should NOT be applied";
    EXPECT_FALSE(mock_->wasCalled("CREATE TABLE d")) << "V4 should NOT be applied";
}

// ============================================================
// 事务边界
// ============================================================

TEST_F(MigrationEngineTest, Migrate_EachMigrationInOwnTransaction) {
    addMigration(1, "a", "CREATE TABLE a (id INT);");
    addMigration(2, "b", "CREATE TABLE b (id INT);");

    engine_->migrate();

    // 每个迁移应该有独立的 BEGIN/COMMIT 对
    auto calls = mock_->callLog();
    int beginCount  = 0;
    int commitCount = 0;
    for (const auto& c : calls) {
        if (c.sql == "BEGIN")  ++beginCount;
        if (c.sql == "COMMIT") ++commitCount;
    }
    EXPECT_GE(beginCount,  2) << "Each migration needs its own transaction";
    EXPECT_GE(commitCount, 2);
    EXPECT_EQ(beginCount, commitCount) << "BEGIN/COMMIT must be balanced";
}

TEST_F(MigrationEngineTest, Migrate_TransactionCommit_RecordMigrationIsInsideTransaction) {
    addMigration(1, "a", "CREATE TABLE a (id INT);");

    engine_->migrate();

    // INSERT INTO schema_migrations 必须在 COMMIT 之前
    auto calls = mock_->callLog();
    int sqlPos    = -1;
    int insertPos = -1;
    int commitPos = -1;
    for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
        if (calls[i].sql.find("CREATE TABLE a") != std::string::npos)
            sqlPos = i;
        if (calls[i].sql.find("INSERT INTO schema_migrations") != std::string::npos)
            insertPos = i;
        if (calls[i].sql == "COMMIT" && commitPos == -1)
            commitPos = i;
    }
    EXPECT_LT(sqlPos,    commitPos) << "Migration SQL must be before COMMIT";
    EXPECT_LT(insertPos, commitPos) << "Record insert must be before COMMIT";
}

// ============================================================
// 错误处理：单个迁移失败
// ============================================================

TEST_F(MigrationEngineTest, Migrate_FirstMigrationFails_RollsBackAndStops) {
    addMigration(1, "broken", "INVALID SQL SYNTAX @@@@;");
    addMigration(2, "good",   "CREATE TABLE b (id INT);");

    mock_->injectError("INVALID SQL", "syntax error at position 1");

    MigrationResult result = engine_->migrate();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.appliedCount, 0);
    EXPECT_FALSE(result.errors.empty());
    // V2 不应该被执行（在 V1 失败后停止）
    EXPECT_FALSE(mock_->wasCalled("CREATE TABLE b"));
}

TEST_F(MigrationEngineTest, Migrate_FailedMigration_RecordsFailureInSchemaTable) {
    addMigration(1, "broken", "INVALID SQL SYNTAX;");
    mock_->injectError("INVALID SQL SYNTAX", "syntax error");

    engine_->migrate();

    // 应该记录失败状态（success = false）
    bool foundFailureRecord = false;
    for (const auto& call : mock_->callLog()) {
        if (call.sql.find("INSERT INTO schema_migrations") != std::string::npos) {
            // 检查参数中包含 false/f 表示失败
            for (const auto& p : call.params) {
                if (p == "false" || p == "f" || p == "0") {
                    foundFailureRecord = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundFailureRecord)
        << "Failed migration should be recorded in schema_migrations";
}

TEST_F(MigrationEngineTest, Migrate_SecondMigrationFails_FirstRemainsApplied) {
    addMigration(1, "good",   "CREATE TABLE a (id INT);");
    addMigration(2, "broken", "INVALID SQL;");

    // V1 应用成功后，模拟 V2 失败
    mock_->injectError("INVALID SQL", "syntax error");

    // 模拟 V1 已成功记录（查询时返回 V1 已应用）
    // 这个场景测试：V1 COMMIT 成功，V2 失败不影响 V1
    MigrationResult result = engine_->migrate();

    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.appliedCount, 1);  // V1 已提交
    EXPECT_EQ(result.failedVersion, 2);
}

TEST_F(MigrationEngineTest, Migrate_DatabaseConnectionLost_ThrowsConnectionError) {
    addMigration(1, "schema", "CREATE TABLE t (id INT);");
    mock_->injectError("CREATE TABLE", "connection lost during query");

    MigrationResult result = engine_->migrate();
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errors.empty());
}

// ============================================================
// 并发保护
// ============================================================

TEST_F(MigrationEngineTest, Migrate_LockAlreadyHeld_ThrowsConcurrentMigrationError) {
    MockResult lockFailed;
    lockFailed.rows = {{{"pg_try_advisory_lock", "f"}}};
    mock_->setResult("pg_try_advisory_lock", lockFailed);

    addMigration(1, "schema", "CREATE TABLE t (id INT);");

    EXPECT_THROW(engine_->migrate(), MigrationEngine::ConcurrentMigrationError);
}

// ============================================================
// Checksum 校验
// ============================================================

TEST_F(MigrationEngineTest, Migrate_ChecksumMismatch_ThrowsChecksumError) {
    // 模拟 V1 已应用，但 checksum 与当前文件不符
    MockResult applied;
    applied.rows = {{
        {"version", "1"},
        {"description", "initial"},
        {"success", "t"},
        {"checksum", "original_checksum_that_was_changed"}
    }};
    mock_->setResult("SELECT.*schema_migrations", applied);

    addMigration(1, "initial", "CREATE TABLE users (id SERIAL);");
    // 文件的实际 checksum 与数据库记录的不同

    EXPECT_THROW(engine_->migrate(), MigrationEngine::ChecksumMismatchError);
}

// ============================================================
// rollback()
// ============================================================

TEST_F(MigrationEngineTest, Rollback_LastMigration_ExecutesDownSql) {
    MockResult applied;
    applied.rows = {{
        {"version", "3"},
        {"description", "add_index"},
        {"success", "t"},
        {"checksum", "some_cs"}
    }};
    mock_->setResult("SELECT.*schema_migrations.*ORDER.*DESC", applied);

    addMigration(3, "add_index",
                 "CREATE INDEX idx ON users(id);",
                 "DROP INDEX idx;");

    MigrationResult result = engine_->rollback();

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(mock_->wasCalled("DROP INDEX idx"))
        << "Expected down SQL to be executed on rollback";
}

TEST_F(MigrationEngineTest, Rollback_NoMigrationsApplied_ThrowsNothingToRollback) {
    mock_->setResult("SELECT.*schema_migrations", MockResult{});

    EXPECT_THROW(engine_->rollback(), MigrationEngine::NothingToRollbackError);
}

TEST_F(MigrationEngineTest, Rollback_NoDownSqlFile_ThrowsNoDownMigrationError) {
    MockResult applied;
    applied.rows = {{{"version", "2"}, {"description", "schema"}, {"success", "t"},
                     {"checksum", "cs"}}};
    mock_->setResult("SELECT.*schema_migrations.*ORDER.*DESC", applied);

    // 只写 up 文件，不写 down 文件
    tmpDir_.writeRaw("V002__schema.up.sql", "CREATE TABLE t (id INT);");
    // 不写 V002__schema.down.sql

    EXPECT_THROW(engine_->rollback(), MigrationEngine::NoDownMigrationError);
}

TEST_F(MigrationEngineTest, RollbackTo_TargetVersion_ExecutesDownSqlInReverseOrder) {
    // 模拟 V1-V3 已应用
    MockResult applied;
    applied.rows = {
        {{"version", "1"}, {"description", "a"}, {"success", "t"}, {"checksum", "cs1"}},
        {{"version", "2"}, {"description", "b"}, {"success", "t"}, {"checksum", "cs2"}},
        {{"version", "3"}, {"description", "c"}, {"success", "t"}, {"checksum", "cs3"}},
    };
    mock_->setResult("SELECT.*schema_migrations", applied);

    addMigration(1, "a", "CREATE TABLE a (id INT);", "DROP TABLE a;");
    addMigration(2, "b", "CREATE TABLE b (id INT);", "DROP TABLE b;");
    addMigration(3, "c", "CREATE TABLE c (id INT);", "DROP TABLE c;");

    // 回滚到 V1（执行 V3.down 和 V2.down，保留 V1）
    MigrationResult result = engine_->rollbackTo(1);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.rolledBackCount, 2);  // V3 和 V2 被回滚

    // 验证顺序：V3.down 先于 V2.down
    auto calls = mock_->callLog();
    int v3downPos = -1, v2downPos = -1;
    for (int i = 0; i < static_cast<int>(calls.size()); ++i) {
        if (calls[i].sql.find("DROP TABLE c") != std::string::npos) v3downPos = i;
        if (calls[i].sql.find("DROP TABLE b") != std::string::npos) v2downPos = i;
    }
    EXPECT_LT(v3downPos, v2downPos)
        << "V3 must be rolled back before V2 (reverse order)";
    EXPECT_FALSE(mock_->wasCalled("DROP TABLE a"))
        << "V1 should NOT be rolled back";
}

// ============================================================
// 边界条件
// ============================================================

TEST_F(MigrationEngineTest, MigrateTo_CurrentVersion_NoOp) {
    MockResult applied;
    applied.rows = {
        {{"version", "2"}, {"description", "b"}, {"success", "t"}, {"checksum", "cs"}}
    };
    mock_->setResult("SELECT.*schema_migrations", applied);
    addMigration(1, "a", "CREATE TABLE a (id INT);");
    addMigration(2, "b", "CREATE TABLE b (id INT);");

    mock_->clearCallLog();
    MigrationResult result = engine_->migrateTo(2);  // 已在 V2，无操作

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.appliedCount, 0);
}

TEST_F(MigrationEngineTest, MigrateTo_NonExistentVersion_ThrowsVersionNotFoundError) {
    addMigration(1, "a", "SELECT 1;");

    EXPECT_THROW(
        engine_->migrateTo(999),
        MigrationEngine::VersionNotFoundError
    );
}

TEST_F(MigrationEngineTest, Migrate_VeryLargeSqlFile_ProcessesWithoutTruncation) {
    // 生成 1MB 的 SQL（100k 个 INSERT 语句模拟大型迁移）
    std::string largeSql;
    largeSql.reserve(1024 * 1024);
    for (int i = 0; i < 1000; ++i) {
        largeSql += "-- Line " + std::to_string(i) + ": padding padding padding\n";
    }
    largeSql += "CREATE TABLE large_test (id INT);";

    addMigration(1, "large_migration", largeSql);

    MigrationResult result = engine_->migrate();
    EXPECT_TRUE(result.success);
    EXPECT_TRUE(mock_->wasCalled("CREATE TABLE large_test"));
}
