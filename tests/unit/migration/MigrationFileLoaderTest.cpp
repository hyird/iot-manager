/**
 * @file MigrationFileLoaderTest.cpp
 * @brief 迁移文件加载器单元测试
 *
 * 测试覆盖：
 *   - 从目录扫描并加载迁移文件
 *   - SQL 内容读取（UTF-8、多行、特殊字符）
 *   - Checksum 计算（SHA-256，幂等验证）
 *   - 错误处理（目录不存在、文件权限、文件为空）
 *   - 文件过滤（只加载 .sql 文件，忽略其他）
 *
 * 依赖：TempMigrationDir（测试隔离用临时目录）
 */

#include <gtest/gtest.h>
#include "migration/MigrationFileLoader.hpp"   // 待实现
#include "helpers/TestDatabaseHelper.hpp"

using namespace iot::migration;
using namespace iot::test;

class MigrationFileLoaderTest : public ::testing::Test {
protected:
    TempMigrationDir tmpDir_;
};

// ============================================================
// 正常路径：文件扫描与加载
// ============================================================

TEST_F(MigrationFileLoaderTest, Scan_EmptyDirectory_ReturnsEmptyList) {
    MigrationFileLoader loader(tmpDir_.path());
    auto files = loader.scan();
    EXPECT_TRUE(files.empty());
}

TEST_F(MigrationFileLoaderTest, Scan_SingleUpFile_ReturnsOneEntry) {
    tmpDir_.write({1, "initial_schema",
                   "CREATE TABLE test (id SERIAL PRIMARY KEY);", ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto files = loader.scan();

    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].version, 1);
    EXPECT_EQ(files[0].description, "initial_schema");
    EXPECT_TRUE(files[0].isUp);
}

TEST_F(MigrationFileLoaderTest, Scan_UpAndDownFiles_ReturnsBothEntries) {
    tmpDir_.write({1, "initial_schema",
                   "CREATE TABLE test (id SERIAL);",
                   "DROP TABLE test;"});

    MigrationFileLoader loader(tmpDir_.path());
    auto files = loader.scan();

    ASSERT_EQ(files.size(), 2u);

    // 验证 up 和 down 都被加载
    bool hasUp = false, hasDown = false;
    for (const auto& f : files) {
        if (f.isUp)  hasUp  = true;
        if (!f.isUp) hasDown = true;
    }
    EXPECT_TRUE(hasUp);
    EXPECT_TRUE(hasDown);
}

TEST_F(MigrationFileLoaderTest, Scan_MultipleVersions_ReturnsSortedAscending) {
    tmpDir_.write({3, "third",  "-- v3", ""});
    tmpDir_.write({1, "first",  "-- v1", ""});
    tmpDir_.write({2, "second", "-- v2", ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto upFiles = loader.scanUpMigrations();

    ASSERT_EQ(upFiles.size(), 3u);
    EXPECT_EQ(upFiles[0].version, 1);
    EXPECT_EQ(upFiles[1].version, 2);
    EXPECT_EQ(upFiles[2].version, 3);
}

TEST_F(MigrationFileLoaderTest, Scan_NonSqlFiles_AreIgnored) {
    tmpDir_.write({1, "schema", "CREATE TABLE t (id INT);", ""});
    tmpDir_.writeRaw("README.md",     "# Migrations");
    tmpDir_.writeRaw("schema.json",   "{}");
    tmpDir_.writeRaw("V002__notes",   "some notes");  // 无扩展名

    MigrationFileLoader loader(tmpDir_.path());
    auto files = loader.scan();

    ASSERT_EQ(files.size(), 1u);
}

TEST_F(MigrationFileLoaderTest, Scan_InvalidFilenames_AreIgnored) {
    tmpDir_.write({1, "valid", "CREATE TABLE t (id INT);", ""});
    tmpDir_.writeRaw("001__no_V_prefix.up.sql", "SELECT 1;");
    tmpDir_.writeRaw("Vinvalid__desc.up.sql",   "SELECT 1;");
    tmpDir_.writeRaw("V0__empty_version.up.sql", "SELECT 1;");

    MigrationFileLoader loader(tmpDir_.path());
    auto files = loader.scan();

    // 只有合法的 V001__valid.up.sql 被加载
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].version, 1);
}

// ============================================================
// SQL 内容读取
// ============================================================

TEST_F(MigrationFileLoaderTest, LoadContent_SimpleSql_ReturnsExactContent) {
    const std::string sql = "CREATE TABLE users (id SERIAL PRIMARY KEY, name TEXT);";
    tmpDir_.write({1, "users", sql, ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto content = loader.loadUpSql(1);

    EXPECT_EQ(content, sql);
}

TEST_F(MigrationFileLoaderTest, LoadContent_MultilineSql_PreservesNewlines) {
    const std::string sql =
        "CREATE TABLE users (\n"
        "    id SERIAL PRIMARY KEY,\n"
        "    name TEXT NOT NULL,\n"
        "    created_at TIMESTAMPTZ DEFAULT NOW()\n"
        ");";
    tmpDir_.write({1, "users", sql, ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto content = loader.loadUpSql(1);

    EXPECT_EQ(content, sql);
}

TEST_F(MigrationFileLoaderTest, LoadContent_SqlWithComments_PreservesComments) {
    const std::string sql =
        "-- V001: 初始化用户表\n"
        "-- Author: DBA\n"
        "CREATE TABLE users (id SERIAL);  -- 主键";
    tmpDir_.write({1, "users", sql, ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto content = loader.loadUpSql(1);

    EXPECT_EQ(content, sql);
}

TEST_F(MigrationFileLoaderTest, LoadContent_SqlWithUnicode_PreservesEncoding) {
    // SQL 注释可以包含中文（合法注释）
    const std::string sql = "-- 创建设备表\nCREATE TABLE device (id SERIAL);";
    tmpDir_.write({1, "device", sql, ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto content = loader.loadUpSql(1);

    EXPECT_EQ(content, sql);
}

TEST_F(MigrationFileLoaderTest, LoadContent_EmptySqlFile_ThrowsOrReturnsEmpty) {
    // 空文件应该报错（空迁移没有意义）
    tmpDir_.writeRaw("V001__empty.up.sql", "");

    MigrationFileLoader loader(tmpDir_.path());

    // 实现选择 A：抛出异常
    EXPECT_THROW(loader.loadUpSql(1), MigrationFileLoader::EmptyMigrationError);
    // 实现选择 B：返回空字符串并让引擎处理（注释掉上面的 EXPECT_THROW）
}

TEST_F(MigrationFileLoaderTest, LoadContent_VersionNotFound_ThrowsNotFoundError) {
    tmpDir_.write({1, "schema", "CREATE TABLE t (id INT);", ""});

    MigrationFileLoader loader(tmpDir_.path());

    EXPECT_THROW(loader.loadUpSql(999), MigrationFileLoader::MigrationNotFoundError);
}

// ============================================================
// Checksum 计算
// ============================================================

TEST_F(MigrationFileLoaderTest, Checksum_SameContent_ReturnsSameHash) {
    const std::string sql = "CREATE TABLE t (id INT);";
    tmpDir_.write({1, "schema", sql, ""});

    MigrationFileLoader loader(tmpDir_.path());

    auto cs1 = loader.computeChecksum(1);
    auto cs2 = loader.computeChecksum(1);

    EXPECT_EQ(cs1, cs2);
}

TEST_F(MigrationFileLoaderTest, Checksum_DifferentContent_ReturnsDifferentHash) {
    tmpDir_.write({1, "schema_a", "CREATE TABLE a (id INT);", ""});
    tmpDir_.write({2, "schema_b", "CREATE TABLE b (id INT);", ""});

    MigrationFileLoader loader(tmpDir_.path());

    auto cs1 = loader.computeChecksum(1);
    auto cs2 = loader.computeChecksum(2);

    EXPECT_NE(cs1, cs2);
}

TEST_F(MigrationFileLoaderTest, Checksum_Format_IsSha256HexString) {
    tmpDir_.write({1, "schema", "CREATE TABLE t (id INT);", ""});

    MigrationFileLoader loader(tmpDir_.path());
    auto checksum = loader.computeChecksum(1);

    // SHA-256 hex 字符串：64 个十六进制字符
    EXPECT_EQ(checksum.size(), 64u);
    EXPECT_TRUE(std::all_of(checksum.begin(), checksum.end(),
        [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
}

// ============================================================
// 错误处理：不存在的目录
// ============================================================

TEST_F(MigrationFileLoaderTest, Constructor_NonExistentDirectory_ThrowsOnScan) {
    MigrationFileLoader loader("/nonexistent/path/that/does/not/exist");

    EXPECT_THROW(loader.scan(), MigrationFileLoader::DirectoryNotFoundError);
}

TEST_F(MigrationFileLoaderTest, Scan_DuplicateVersions_ThrowsDuplicateVersionError) {
    // 同一目录下两个 V001 文件（不同描述）应报错
    tmpDir_.writeRaw("V001__first_attempt.up.sql",  "CREATE TABLE a (id INT);");
    tmpDir_.writeRaw("V001__second_attempt.up.sql", "CREATE TABLE b (id INT);");

    MigrationFileLoader loader(tmpDir_.path());

    EXPECT_THROW(loader.scan(), MigrationFileLoader::DuplicateVersionError);
}

// ============================================================
// 大量文件性能测试（100 个迁移）
// ============================================================

TEST_F(MigrationFileLoaderTest, Scan_100Migrations_CompletesWithin500ms) {
    for (int i = 1; i <= 100; ++i) {
        tmpDir_.write({i, "migration_" + std::to_string(i),
                       "SELECT " + std::to_string(i) + ";", ""});
    }

    MigrationFileLoader loader(tmpDir_.path());

    auto start = std::chrono::steady_clock::now();
    auto files = loader.scan();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(files.size(), 100u);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
}
