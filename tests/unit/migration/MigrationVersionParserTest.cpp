/**
 * @file MigrationVersionParserTest.cpp
 * @brief 迁移版本解析器单元测试
 *
 * 测试覆盖：
 *   - 合法文件名解析（版本号、描述、up/down 区分）
 *   - 非法文件名（格式错误、版本溢出、SQL 注入尝试）
 *   - 版本排序（有序、乱序、重复版本检测）
 *   - 边界条件（version=0、version=MAX_INT、空描述）
 *
 * TDD 红灯阶段：这些测试在 MigrationVersionParser 实现前全部失败。
 * 运行：ctest -R MigrationVersionParser
 */

#include <gtest/gtest.h>
#include "migration/MigrationVersionParser.hpp"   // 待实现

using namespace iot::migration;

// ============================================================
// Fixture：无状态，解析器本身是纯函数
// ============================================================
class MigrationVersionParserTest : public ::testing::Test {};

// ============================================================
// 正常路径：合法文件名解析
// ============================================================

TEST_F(MigrationVersionParserTest, ParseUpFile_ValidFormat_ReturnsCorrectVersion) {
    // Arrange
    const std::string filename = "V001__initial_schema.up.sql";

    // Act
    auto result = MigrationVersionParser::parse(filename);

    // Assert
    ASSERT_TRUE(result.valid) << "Expected valid parse result for: " << filename;
    EXPECT_EQ(result.version, 1);
    EXPECT_EQ(result.description, "initial_schema");
    EXPECT_TRUE(result.isUp);
}

TEST_F(MigrationVersionParserTest, ParseDownFile_ValidFormat_ReturnsCorrectVersion) {
    const std::string filename = "V001__initial_schema.down.sql";
    auto result = MigrationVersionParser::parse(filename);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.version, 1);
    EXPECT_EQ(result.description, "initial_schema");
    EXPECT_FALSE(result.isUp);
}

TEST_F(MigrationVersionParserTest, ParseFile_MultiWordDescription_PreservesUnderscores) {
    // 描述中多个单词用下划线连接
    const std::string filename = "V002__add_agent_node_table.up.sql";
    auto result = MigrationVersionParser::parse(filename);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.version, 2);
    EXPECT_EQ(result.description, "add_agent_node_table");
}

TEST_F(MigrationVersionParserTest, ParseFile_ThreeDigitVersion_ParsesCorrectly) {
    const std::string filename = "V010__add_device_data_hypertable.up.sql";
    auto result = MigrationVersionParser::parse(filename);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.version, 10);
}

TEST_F(MigrationVersionParserTest, ParseFile_LargeVersion_ParsesCorrectly) {
    const std::string filename = "V999__last_migration.up.sql";
    auto result = MigrationVersionParser::parse(filename);

    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.version, 999);
}

// ============================================================
// 异常路径：非法文件名
// ============================================================

TEST_F(MigrationVersionParserTest, ParseFile_MissingVersionPrefix_ReturnsInvalid) {
    EXPECT_FALSE(MigrationVersionParser::parse("001__schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_MissingSeparator_ReturnsInvalid) {
    // 分隔符必须是双下划线 __
    EXPECT_FALSE(MigrationVersionParser::parse("V001_schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001-schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001schema.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_MissingExtension_ReturnsInvalid) {
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schema.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schema").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schema.up").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_NonNumericVersion_ReturnsInvalid) {
    EXPECT_FALSE(MigrationVersionParser::parse("Vabc__schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V__schema.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_EmptyDescription_ReturnsInvalid) {
    // 描述不允许为空
    EXPECT_FALSE(MigrationVersionParser::parse("V001__.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001___.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_EmptyString_ReturnsInvalid) {
    EXPECT_FALSE(MigrationVersionParser::parse("").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_SqlInjectionInFilename_ReturnsInvalid) {
    // 文件名中不允许出现特殊字符（安全验证）
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schema;DROP TABLE users--.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001__../../etc/passwd.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schema\x00inject.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_PathTraversal_ReturnsInvalid) {
    EXPECT_FALSE(MigrationVersionParser::parse("../V001__schema.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("/etc/V001__schema.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_UnicodeDescription_ReturnsInvalid) {
    // 描述只允许 ASCII 字母数字和下划线
    EXPECT_FALSE(MigrationVersionParser::parse("V001__加载数据.up.sql").valid);
    EXPECT_FALSE(MigrationVersionParser::parse("V001__schéma.up.sql").valid);
}

// ============================================================
// 版本排序
// ============================================================

TEST_F(MigrationVersionParserTest, SortVersions_UnsortedInput_ReturnsSortedAscending) {
    std::vector<ParsedMigrationFile> files = {
        {true, 3, "third",  true},
        {true, 1, "first",  true},
        {true, 2, "second", true},
    };

    MigrationVersionParser::sortByVersion(files);

    EXPECT_EQ(files[0].version, 1);
    EXPECT_EQ(files[1].version, 2);
    EXPECT_EQ(files[2].version, 3);
}

TEST_F(MigrationVersionParserTest, SortVersions_AlreadySorted_RemainsUnchanged) {
    std::vector<ParsedMigrationFile> files = {
        {true, 1, "a", true},
        {true, 2, "b", true},
        {true, 3, "c", true},
    };

    MigrationVersionParser::sortByVersion(files);

    EXPECT_EQ(files[0].version, 1);
    EXPECT_EQ(files[1].version, 2);
    EXPECT_EQ(files[2].version, 3);
}

TEST_F(MigrationVersionParserTest, SortVersions_EmptyList_NoError) {
    std::vector<ParsedMigrationFile> files;
    EXPECT_NO_THROW(MigrationVersionParser::sortByVersion(files));
    EXPECT_TRUE(files.empty());
}

TEST_F(MigrationVersionParserTest, SortVersions_SingleElement_NoChange) {
    std::vector<ParsedMigrationFile> files = {{true, 5, "only", true}};
    MigrationVersionParser::sortByVersion(files);
    EXPECT_EQ(files[0].version, 5);
}

// ============================================================
// 重复版本检测
// ============================================================

TEST_F(MigrationVersionParserTest, ValidateDuplicates_NoDuplicates_ReturnsEmpty) {
    std::vector<ParsedMigrationFile> files = {
        {true, 1, "a", true},
        {true, 2, "b", true},
        {true, 3, "c", true},
    };

    auto duplicates = MigrationVersionParser::findDuplicateVersions(files);
    EXPECT_TRUE(duplicates.empty());
}

TEST_F(MigrationVersionParserTest, ValidateDuplicates_HasDuplicates_ReturnsDuplicateVersions) {
    std::vector<ParsedMigrationFile> files = {
        {true, 1, "a",        true},
        {true, 2, "b_first",  true},
        {true, 2, "b_second", true},   // 重复版本 2
        {true, 3, "c",        true},
    };

    auto duplicates = MigrationVersionParser::findDuplicateVersions(files);
    ASSERT_EQ(duplicates.size(), 1u);
    EXPECT_EQ(duplicates[0], 2);
}

TEST_F(MigrationVersionParserTest, ValidateDuplicates_MultipleDuplicates_ReturnsAllDuplicateVersions) {
    std::vector<ParsedMigrationFile> files = {
        {true, 1, "a",  true},
        {true, 1, "aa", true},
        {true, 2, "b",  true},
        {true, 2, "bb", true},
    };

    auto duplicates = MigrationVersionParser::findDuplicateVersions(files);
    EXPECT_EQ(duplicates.size(), 2u);
}

// ============================================================
// 版本间隙检测（可选的连续性验证）
// ============================================================

TEST_F(MigrationVersionParserTest, FindGaps_ConsecutiveVersions_ReturnsEmpty) {
    std::vector<int> versions = {1, 2, 3, 4, 5};
    auto gaps = MigrationVersionParser::findVersionGaps(versions);
    EXPECT_TRUE(gaps.empty());
}

TEST_F(MigrationVersionParserTest, FindGaps_WithGap_ReturnsGapVersions) {
    std::vector<int> versions = {1, 2, 4, 5};   // 缺少 3
    auto gaps = MigrationVersionParser::findVersionGaps(versions);
    ASSERT_EQ(gaps.size(), 1u);
    EXPECT_EQ(gaps[0], 3);
}

TEST_F(MigrationVersionParserTest, FindGaps_MultipleGaps_ReturnsAllMissing) {
    std::vector<int> versions = {1, 3, 6};  // 缺少 2, 4, 5
    auto gaps = MigrationVersionParser::findVersionGaps(versions);
    EXPECT_EQ(gaps.size(), 3u);
}

// ============================================================
// 边界条件
// ============================================================

TEST_F(MigrationVersionParserTest, ParseFile_VersionZero_ReturnsInvalid) {
    // 版本号从 1 开始，0 无效
    EXPECT_FALSE(MigrationVersionParser::parse("V000__schema.up.sql").valid);
}

TEST_F(MigrationVersionParserTest, ParseFile_VeryLongDescription_StillValid) {
    // 256 个字符的描述仍然有效
    std::string longDesc(256, 'a');
    std::string filename = "V001__" + longDesc + ".up.sql";
    auto result = MigrationVersionParser::parse(filename);
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.description, longDesc);
}

TEST_F(MigrationVersionParserTest, ParseFile_DescriptionWithNumbers_IsValid) {
    auto result = MigrationVersionParser::parse("V001__add_v2_columns.up.sql");
    ASSERT_TRUE(result.valid);
    EXPECT_EQ(result.description, "add_v2_columns");
}
