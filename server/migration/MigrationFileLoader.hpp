#pragma once

/**
 * @file MigrationFileLoader.hpp
 * @brief 迁移文件加载器接口
 *
 * 负责：
 *   1. 扫描迁移目录，发现所有合法的 .up.sql / .down.sql 文件
 *   2. 读取 SQL 文件内容（UTF-8）
 *   3. 计算文件 Checksum（SHA-256）
 *
 * 设计约束：
 *   - 纯文件系统操作，不涉及数据库
 *   - scan() 失败时抛出，而非返回错误码
 *   - loadUpSql()/loadDownSql() 在版本不存在时抛出 MigrationNotFoundException
 */

#include "MigrationTypes.hpp"
#include "MigrationVersionParser.hpp"
#include <string>
#include <vector>

namespace iot::migration {

class MigrationFileLoader {
public:
    // 异常类型别名（对外暴露，测试代码使用）
    using EmptyMigrationError     = EmptyMigrationException;
    using MigrationNotFoundError  = MigrationNotFoundException;
    using DuplicateVersionError   = DuplicateVersionException;
    using DirectoryNotFoundError  = DirectoryNotFoundException;

    /**
     * @brief 构造加载器
     * @param migrationsDir  迁移 SQL 文件所在目录路径
     */
    explicit MigrationFileLoader(const std::string& migrationsDir);

    /**
     * @brief 扫描目录，返回所有有效迁移文件（up + down）
     *
     * @throws DirectoryNotFoundError  目录不存在
     * @throws DuplicateVersionError   同一目录下存在重复版本号
     */
    std::vector<ParsedMigrationFile> scan();

    /**
     * @brief 扫描并只返回 up 迁移文件（已按版本升序排列）
     *
     * @throws DirectoryNotFoundError
     * @throws DuplicateVersionError
     */
    std::vector<ParsedMigrationFile> scanUpMigrations();

    /**
     * @brief 加载指定版本的 up SQL 内容
     *
     * @throws MigrationNotFoundError  版本不存在
     * @throws EmptyMigrationError     文件为空
     */
    std::string loadUpSql(int version);

    /**
     * @brief 加载指定版本的 down SQL 内容
     *
     * @throws MigrationNotFoundError   版本不存在
     * @throws NoDownMigrationException 无 down 文件
     * @throws EmptyMigrationError      文件为空
     */
    std::string loadDownSql(int version);

    /**
     * @brief 计算指定版本 up SQL 文件的 SHA-256 checksum（hex 字符串）
     *
     * @throws MigrationNotFoundError  版本不存在
     */
    std::string computeChecksum(int version);

private:
    std::string migrationsDir_;

    std::string readFile(const std::string& path);
    std::string sha256Hex(const std::string& content);
    std::string findFilePath(int version, bool isUp);
};

} // namespace iot::migration
