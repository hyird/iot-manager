#pragma once

/**
 * @file MigrationTypes.hpp
 * @brief 迁移系统公共类型定义
 *
 * 所有迁移组件（Parser、Loader、StateManager、Engine）
 * 共享的数据类型，无外部依赖。
 */

#include <string>
#include <vector>
#include <stdexcept>

namespace iot::migration {

// ============================================================
// 核心数据结构
// ============================================================

/**
 * @brief 解析后的迁移文件元数据
 */
struct ParsedMigrationFile {
    bool        valid       = false;
    int         version     = 0;
    std::string description;
    bool        isUp        = false;  ///< true = .up.sql, false = .down.sql
};

/**
 * @brief schema_migrations 表中的一条记录
 */
struct MigrationRecord {
    int         version;
    std::string description;
    std::string checksum;       ///< SHA-256 hex of up SQL content
    std::string appliedAt;      ///< TIMESTAMPTZ as ISO 8601 string
    bool        success;
};

/**
 * @brief migrate() / rollback() 的执行结果
 */
struct MigrationResult {
    bool                     success        = false;
    int                      appliedCount   = 0;
    int                      rolledBackCount = 0;
    int                      failedVersion  = -1;   ///< -1 = 无失败
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

// ============================================================
// 异常类型层次
// ============================================================

/// 所有迁移异常的基类
class MigrationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 迁移文件未找到
class MigrationNotFoundException : public MigrationError {
public:
    explicit MigrationNotFoundException(int version)
        : MigrationError("Migration version not found: " + std::to_string(version))
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

/// 迁移文件为空
class EmptyMigrationException : public MigrationError {
public:
    using MigrationError::MigrationError;
};

/// 版本重复
class DuplicateVersionException : public MigrationError {
public:
    explicit DuplicateVersionException(int version)
        : MigrationError("Duplicate migration version: " + std::to_string(version))
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

/// 目录不存在
class DirectoryNotFoundException : public MigrationError {
public:
    using MigrationError::MigrationError;
};

/// Checksum 不匹配（文件被篡改）
class ChecksumMismatchException : public MigrationError {
public:
    ChecksumMismatchException(int version,
                               const std::string& expected,
                               const std::string& actual)
        : MigrationError("Checksum mismatch for version " +
                         std::to_string(version) +
                         ": expected=" + expected + " actual=" + actual)
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

/// 并发迁移冲突
class ConcurrentMigrationException : public MigrationError {
public:
    ConcurrentMigrationException()
        : MigrationError("Another migration process is running") {}
};

/// 无可回滚的迁移
class NothingToRollbackException : public MigrationError {
public:
    NothingToRollbackException()
        : MigrationError("No applied migrations to rollback") {}
};

/// 指定版本无 down 迁移文件
class NoDownMigrationException : public MigrationError {
public:
    explicit NoDownMigrationException(int version)
        : MigrationError("No down migration file for version " +
                         std::to_string(version))
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

/// 目标版本不存在
class VersionNotFoundException : public MigrationError {
public:
    explicit VersionNotFoundException(int version)
        : MigrationError("Version not found: " + std::to_string(version))
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

/// 迁移版本未应用（查询 checksum 时）
class MigrationNotAppliedException : public MigrationError {
public:
    explicit MigrationNotAppliedException(int version)
        : MigrationError("Migration not applied: " + std::to_string(version))
        , version_(version) {}
    int version() const { return version_; }
private:
    int version_;
};

} // namespace iot::migration
