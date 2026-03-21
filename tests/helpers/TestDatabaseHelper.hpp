#pragma once

/**
 * @file TestDatabaseHelper.hpp
 * @brief 测试数据库辅助工具
 *
 * 提供测试隔离、连接管理、Schema 清理等基础设施。
 * 每个测试套件使用独立的 PostgreSQL Schema 实现隔离，
 * 避免 DROP DATABASE 导致的高权限需求和慢速操作。
 *
 * 使用方式：
 *   每个集成测试 fixture 继承 DatabaseTestFixture，
 *   或通过 TestDatabaseHelper::create() 手动管理。
 */

#include <string>
#include <memory>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <sstream>

// ============================================================
// 前向声明：使用真实 libpq 时替换为 #include <libpq-fe.h>
// 在 mock 场景下这些由 MockDbConnection 替代
// ============================================================
struct pg_conn;
using PGconn = pg_conn;

namespace iot::test {

/**
 * @brief 测试数据库连接参数
 *
 * 优先读取环境变量，否则使用本地开发默认值。
 * CI/CD 中通过环境变量注入隔离的测试数据库。
 */
struct TestDbConfig {
    std::string host     = getEnvOrDefault("TEST_DB_HOST", "localhost");
    std::string port     = getEnvOrDefault("TEST_DB_PORT", "5432");
    std::string dbname   = getEnvOrDefault("TEST_DB_NAME", "iot_manager_test");
    std::string user     = getEnvOrDefault("TEST_DB_USER", "postgres");
    std::string password = getEnvOrDefault("TEST_DB_PASSWORD", "postgres");

    std::string toConnString() const {
        return "host=" + host +
               " port=" + port +
               " dbname=" + dbname +
               " user=" + user +
               " password=" + password +
               " connect_timeout=5";
    }

    static std::string getEnvOrDefault(const char* name, const char* defaultVal) {
        const char* val = std::getenv(name);
        return val ? std::string(val) : std::string(defaultVal);
    }
};

/**
 * @brief 单个迁移文件的描述符
 *
 * 对应文件系统上的 V{version}__{description}.up.sql
 * 和 V{version}__{description}.down.sql 文件对。
 */
struct MigrationFileDescriptor {
    int         version;        // 版本号（如 1, 2, 3）
    std::string description;    // 描述（如 "initial_schema"）
    std::string upSql;          // up 迁移 SQL 内容
    std::string downSql;        // down 迁移 SQL 内容（可为空）

    std::string upFileName() const {
        return "V" + std::to_string(version) + "__" + description + ".up.sql";
    }
    std::string downFileName() const {
        return "V" + std::to_string(version) + "__" + description + ".down.sql";
    }
};

/**
 * @brief 迁移记录（对应 schema_migrations 表中的一行）
 */
struct MigrationRecord {
    int         version;
    std::string description;
    std::string checksum;       // SHA-256 of upSql
    std::string appliedAt;      // TIMESTAMPTZ as string
    bool        success;
};

/**
 * @brief 测试用迁移目录管理器
 *
 * 在临时目录中创建/清理 SQL 文件，用于测试文件加载逻辑。
 * RAII 设计：析构时自动删除临时目录。
 */
class TempMigrationDir {
public:
    explicit TempMigrationDir(const std::string& prefix = "iot_migration_test_") {
        namespace fs = std::filesystem;
        path_ = fs::temp_directory_path() / (prefix + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }

    ~TempMigrationDir() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove_all(path_, ec);
        // 析构中忽略错误
    }

    // 禁止复制，允许移动
    TempMigrationDir(const TempMigrationDir&) = delete;
    TempMigrationDir& operator=(const TempMigrationDir&) = delete;

    /**
     * @brief 向临时目录写入迁移文件
     */
    void write(const MigrationFileDescriptor& desc) const {
        namespace fs = std::filesystem;
        writeFile(path_ / desc.upFileName(), desc.upSql);
        if (!desc.downSql.empty()) {
            writeFile(path_ / desc.downFileName(), desc.downSql);
        }
    }

    /**
     * @brief 写入任意文件（用于测试非法文件名等场景）
     */
    void writeRaw(const std::string& filename, const std::string& content) const {
        writeFile(path_ / filename, content);
    }

    std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;

    static void writeFile(const std::filesystem::path& p, const std::string& content) {
        std::ofstream f(p);
        if (!f) throw std::runtime_error("Cannot create file: " + p.string());
        f << content;
    }
};

/**
 * @brief 迁移版本解析结果
 */
struct ParsedMigrationFile {
    bool        valid = false;
    int         version = 0;
    std::string description;
    bool        isUp = false;   // true = .up.sql, false = .down.sql

    static ParsedMigrationFile parse(const std::string& filename);
};

} // namespace iot::test
