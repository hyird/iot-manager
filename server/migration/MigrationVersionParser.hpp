#pragma once

/**
 * @file MigrationVersionParser.hpp
 * @brief 迁移文件名解析器接口
 *
 * 文件名格式：V{version}__{description}.{up|down}.sql
 *   - version：正整数（>= 1），允许前导零（V001 = version 1）
 *   - description：仅含字母、数字、下划线
 *   - 类型：up 或 down
 *
 * 此头文件定义接口契约，实现在 MigrationVersionParser.cpp。
 * 测试先于实现：MigrationVersionParserTest.cpp 在此文件存在后
 * 可编译，但测试会失败（红灯），直到 .cpp 实现完成。
 */

#include "MigrationTypes.hpp"
#include <vector>
#include <string>

namespace iot::migration {

class MigrationVersionParser {
public:
    /**
     * @brief 解析迁移文件名
     * @param filename  仅文件名部分（不含路径）
     * @return 解析结果。result.valid == false 表示格式不合法
     */
    static ParsedMigrationFile parse(const std::string& filename);

    /**
     * @brief 按版本号升序排序（原地修改）
     */
    static void sortByVersion(std::vector<ParsedMigrationFile>& files);

    /**
     * @brief 找出重复的版本号列表
     * @return 有重复的版本号（每个重复版本号出现一次）
     */
    static std::vector<int> findDuplicateVersions(
        const std::vector<ParsedMigrationFile>& files);

    /**
     * @brief 找出版本号序列中的空缺
     * @param versions  已排序的版本号列表
     * @return 缺失的版本号列表
     */
    static std::vector<int> findVersionGaps(const std::vector<int>& versions);
};

} // namespace iot::migration
