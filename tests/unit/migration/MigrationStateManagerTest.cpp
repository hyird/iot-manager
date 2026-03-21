/**
 * @file MigrationStateManagerTest.cpp
 * @brief 迁移状态管理器单元测试（使用 MockDbClient）
 *
 * 测试覆盖：
 *   - schema_migrations 表的创建
 *   - 已应用版本的查询
 *   - 版本记录的插入/更新
 *   - Checksum 验证（检测文件被篡改）
 *   - 并发锁（Advisory Lock）的获取与释放
 *   - 状态机：pending -> running -> applied/failed
 *
 * 这是纯单元测试：所有 SQL 操作通过 MockDbClient 拦截，
 * 不需要真实数据库连接。
 */

#include <gtest/gtest.h>
#include "migration/MigrationStateManager.hpp"   // 待实现
#include "helpers/MockDbClient.hpp"

using namespace iot::migration;
using namespace iot::test;

class MigrationStateManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mock_ = std::make_shared<MockDbClient>();
        manager_ = std::make_unique<MigrationStateManager>(mock_);
    }

    void TearDown() override {
        mock_->reset();
    }

    std::shared_ptr<MockDbClient> mock_;
    std::unique_ptr<MigrationStateManager> manager_;
};

// ============================================================
// schema_migrations 表初始化
// ============================================================

TEST_F(MigrationStateManagerTest, EnsureTable_CreatesSchemaIfNotExists) {
    // Act
    manager_->ensureMigrationTable();

    // Assert：验证执行了 CREATE TABLE IF NOT EXISTS schema_migrations
    EXPECT_TRUE(mock_->wasCalled("CREATE TABLE IF NOT EXISTS schema_migrations"))
        << "Expected CREATE TABLE statement for schema_migrations";
}

TEST_F(MigrationStateManagerTest, EnsureTable_CreatesIndexOnVersion) {
    manager_->ensureMigrationTable();

    EXPECT_TRUE(mock_->wasCalled("CREATE.*INDEX.*schema_migrations.*version"))
        << "Expected index on version column";
}

TEST_F(MigrationStateManagerTest, EnsureTable_IsIdempotent) {
    // 多次调用不应报错（IF NOT EXISTS 语义）
    EXPECT_NO_THROW({
        manager_->ensureMigrationTable();
        manager_->ensureMigrationTable();
        manager_->ensureMigrationTable();
    });
}

// ============================================================
// 查询已应用版本
// ============================================================

TEST_F(MigrationStateManagerTest, GetAppliedVersions_EmptyTable_ReturnsEmptySet) {
    // mock: SELECT 返回空结果集
    mock_->setResult("SELECT.*schema_migrations", MockResult{});

    auto applied = manager_->getAppliedVersions();

    EXPECT_TRUE(applied.empty());
}

TEST_F(MigrationStateManagerTest, GetAppliedVersions_WithRecords_ReturnsVersionSet) {
    MockResult result;
    result.rows = {
        {{"version", "1"}, {"description", "initial_schema"}, {"success", "t"}},
        {{"version", "2"}, {"description", "add_device_table"}, {"success", "t"}},
        {{"version", "3"}, {"description", "add_index"}, {"success", "t"}},
    };
    mock_->setResult("SELECT.*schema_migrations", result);

    auto applied = manager_->getAppliedVersions();

    EXPECT_EQ(applied.size(), 3u);
    EXPECT_TRUE(applied.count(1));
    EXPECT_TRUE(applied.count(2));
    EXPECT_TRUE(applied.count(3));
}

TEST_F(MigrationStateManagerTest, GetAppliedVersions_OnlySuccessfulRecords_ExcludesFailures) {
    MockResult result;
    result.rows = {
        {{"version", "1"}, {"description", "a"}, {"success", "t"}},
        {{"version", "2"}, {"description", "b"}, {"success", "f"}},  // 失败的不算
    };
    mock_->setResult("SELECT.*schema_migrations", result);

    auto applied = manager_->getAppliedVersions();

    EXPECT_TRUE(applied.count(1));
    EXPECT_FALSE(applied.count(2));  // 失败记录不在已应用集合中
}

// ============================================================
// 版本记录写入
// ============================================================

TEST_F(MigrationStateManagerTest, RecordMigration_Success_InsertsRecord) {
    MigrationRecord rec{1, "initial_schema", "abc123checksum", "", true};

    manager_->recordMigration(rec);

    EXPECT_TRUE(mock_->wasCalled("INSERT INTO schema_migrations"))
        << "Expected INSERT for migration record";
}

TEST_F(MigrationStateManagerTest, RecordMigration_Success_IncludesChecksum) {
    MigrationRecord rec{1, "schema", "sha256checksum_value_here", "", true};

    manager_->recordMigration(rec);

    // 验证 checksum 被包含在 INSERT 参数中
    auto calls = mock_->callLog();
    bool found = false;
    for (const auto& call : calls) {
        if (call.sql.find("INSERT INTO schema_migrations") != std::string::npos) {
            for (const auto& p : call.params) {
                if (p == "sha256checksum_value_here") { found = true; break; }
            }
        }
    }
    EXPECT_TRUE(found) << "Checksum not found in INSERT parameters";
}

TEST_F(MigrationStateManagerTest, RecordMigration_Failure_RecordsFailureStatus) {
    MigrationRecord rec{2, "broken_migration", "checksum", "", false};

    manager_->recordMigration(rec);

    auto calls = mock_->callLog();
    bool foundInsert = std::any_of(calls.begin(), calls.end(), [](const auto& c) {
        return c.sql.find("INSERT INTO schema_migrations") != std::string::npos;
    });
    EXPECT_TRUE(foundInsert);
}

TEST_F(MigrationStateManagerTest, RecordMigration_DatabaseError_PropagatesException) {
    mock_->injectError("INSERT INTO schema_migrations", "Connection lost", 1);
    MigrationRecord rec{1, "schema", "cs", "", true};

    EXPECT_THROW(manager_->recordMigration(rec), std::runtime_error);
}

// ============================================================
// Checksum 验证（防止迁移文件被篡改）
// ============================================================

TEST_F(MigrationStateManagerTest, VerifyChecksum_Matches_NoException) {
    MockResult result;
    result.rows = {{{"checksum", "correct_checksum_abc"}}};
    mock_->setResult("SELECT checksum FROM schema_migrations", result);

    EXPECT_NO_THROW(
        manager_->verifyChecksum(1, "correct_checksum_abc")
    );
}

TEST_F(MigrationStateManagerTest, VerifyChecksum_Mismatch_ThrowsChecksumError) {
    MockResult result;
    result.rows = {{{"checksum", "original_checksum"}}};
    mock_->setResult("SELECT checksum FROM schema_migrations", result);

    EXPECT_THROW(
        manager_->verifyChecksum(1, "tampered_checksum"),
        MigrationStateManager::ChecksumMismatchError
    );
}

TEST_F(MigrationStateManagerTest, VerifyChecksum_RecordNotFound_ThrowsNotFoundError) {
    mock_->setResult("SELECT checksum FROM schema_migrations", MockResult{});

    EXPECT_THROW(
        manager_->verifyChecksum(999, "any_checksum"),
        MigrationStateManager::MigrationNotAppliedError
    );
}

// ============================================================
// 并发锁（PostgreSQL Advisory Lock）
// ============================================================

TEST_F(MigrationStateManagerTest, AcquireLock_CallsAdvisoryLock) {
    manager_->acquireLock();

    EXPECT_TRUE(mock_->wasCalled("pg_try_advisory_lock|SELECT.*advisory_lock"))
        << "Expected advisory lock acquisition";
}

TEST_F(MigrationStateManagerTest, AcquireLock_LockAlreadyHeld_ThrowsConcurrentMigrationError) {
    // 模拟锁已被其他进程持有（pg_try_advisory_lock 返回 false）
    MockResult lockResult;
    lockResult.rows = {{{"pg_try_advisory_lock", "f"}}};
    mock_->setResult("pg_try_advisory_lock", lockResult);

    EXPECT_THROW(
        manager_->acquireLock(),
        MigrationStateManager::ConcurrentMigrationError
    );
}

TEST_F(MigrationStateManagerTest, ReleaseLock_CallsAdvisoryUnlock) {
    manager_->releaseLock();

    EXPECT_TRUE(mock_->wasCalled("pg_advisory_unlock"))
        << "Expected advisory lock release";
}

TEST_F(MigrationStateManagerTest, LockRaii_AcquireAndRelease_CorrectOrder) {
    {
        auto lockGuard = manager_->acquireLockGuard();
        // 锁已获取
        EXPECT_TRUE(mock_->wasCalled("pg_try_advisory_lock|advisory_lock"));
    }
    // 离开作用域时自动释放
    EXPECT_TRUE(mock_->wasCalled("pg_advisory_unlock"));
}

// ============================================================
// 获取待应用迁移列表
// ============================================================

TEST_F(MigrationStateManagerTest, GetPendingMigrations_NoneApplied_ReturnsAll) {
    mock_->setResult("SELECT.*schema_migrations", MockResult{});

    std::vector<int> available = {1, 2, 3};
    auto pending = manager_->getPendingVersions(available);

    EXPECT_EQ(pending, available);
}

TEST_F(MigrationStateManagerTest, GetPendingMigrations_SomeApplied_ReturnsOnlyPending) {
    MockResult applied;
    applied.rows = {
        {{"version", "1"}, {"success", "t"}},
        {{"version", "2"}, {"success", "t"}},
    };
    mock_->setResult("SELECT.*schema_migrations", applied);

    std::vector<int> available = {1, 2, 3, 4};
    auto pending = manager_->getPendingVersions(available);

    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending[0], 3);
    EXPECT_EQ(pending[1], 4);
}

TEST_F(MigrationStateManagerTest, GetPendingMigrations_AllApplied_ReturnsEmpty) {
    MockResult applied;
    applied.rows = {
        {{"version", "1"}, {"success", "t"}},
        {{"version", "2"}, {"success", "t"}},
    };
    mock_->setResult("SELECT.*schema_migrations", applied);

    std::vector<int> available = {1, 2};
    auto pending = manager_->getPendingVersions(available);

    EXPECT_TRUE(pending.empty());
}

// ============================================================
// 删除迁移记录（用于回滚后清除记录）
// ============================================================

TEST_F(MigrationStateManagerTest, RemoveMigrationRecord_CallsDelete) {
    manager_->removeMigrationRecord(3);

    EXPECT_TRUE(mock_->wasCalled("DELETE FROM schema_migrations.*version.*3|"
                                  "DELETE FROM schema_migrations.*\\$1"))
        << "Expected DELETE for version 3";
}

TEST_F(MigrationStateManagerTest, RemoveMigrationRecord_DatabaseError_PropagatesException) {
    mock_->injectError("DELETE FROM schema_migrations", "Disk full", 1);

    EXPECT_THROW(manager_->removeMigrationRecord(1), std::runtime_error);
}
