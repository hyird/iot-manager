#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/database/RedisService.hpp"

namespace fs = std::filesystem;

/**
 * @brief 配置管理器 - 负责加载、验证和管理应用配置
 *
 * 启动时检查配置文件的完整性和正确性：
 * - JSON 语法检查
 * - 必填字段检查（listeners、db_clients、redis_clients、jwt）
 * - 端口范围、类型合法性校验
 * - 占位符值警告（YOUR_*、CHANGE_ME 等）
 * - JWT 密钥安全性警告
 */
class ConfigManager {
public:
    /**
     * @brief 加载并验证配置文件
     * @return 是否成功加载，失败时已输出详细错误信息到 stderr 和日志
     */
    static bool load() {
        // 每次加载前重置为默认值，避免读取失败时沿用旧值
        AppDbConfig::useFast() = false;
        AppRedisConfig::useFast() = false;
        numberOfThreads_ = 0;

        // 1. 查找配置文件
        auto configPath = findConfigFile();
        if (!configPath) {
            return false;
        }

        // 2. 解析 JSON
        Json::Value root;
        if (!parseConfigFile(*configPath, root)) {
            return false;
        }

        // 3. 验证配置
        if (!validateConfig(root, *configPath)) {
            return false;
        }

        // 4. 提取自定义配置（is_fast、线程数等）
        applyConfig(root);

        // 5. 加载到 Drogon 框架
        try {
            drogon::app().loadConfigFile(*configPath);
        } catch (const std::exception& e) {
            printErrors("Drogon 加载配置失败", {e.what()});
            return false;
        }

        LOG_INFO << "Config loaded from: " << *configPath;
        return true;
    }

    /**
     * @brief 获取日志级别配置
     */
    static std::string getLogLevel() {
        auto& config = drogon::app().getCustomConfig();
        return config.get("log_level", "INFO").asString();
    }

    /**
     * @brief 获取是否启用控制台日志
     */
    static bool isConsoleLogEnabled() {
        auto& config = drogon::app().getCustomConfig();
        return config.get("console_log", false).asBool();
    }

    /**
     * @brief 获取线程数配置
     * @return 线程数，0 表示自动（使用 CPU 核心数）
     */
    static size_t getNumberOfThreads() {
        return numberOfThreads_;
    }

private:
    inline static size_t numberOfThreads_ = 0;

    // ─── 配置文件查找 ───────────────────────────────────────────

    static std::optional<std::string> findConfigFile() {
        static const std::vector<std::string> paths = {
            "./config/config.local.json",
            "../../config/config.local.json",
            "../config/config.local.json",
            "./config/config.json",
            "../../config/config.json",
            "../config/config.json",
            "config.json"
        };

        for (const auto& path : paths) {
            if (fs::exists(path)) {
                return path;
            }
        }

        std::vector<std::string> hints = {"请在以下位置之一创建配置文件:"};
        for (const auto& p : paths) {
            hints.push_back("  - " + p);
        }
        hints.emplace_back("可参考 config/config.local.example.json");
        printErrors("未找到配置文件", hints);
        return std::nullopt;
    }

    // ─── JSON 解析 ──────────────────────────────────────────────

    static bool parseConfigFile(const std::string& path, Json::Value& root) {
        std::ifstream ifs(path);
        if (!ifs) {
            printErrors("无法打开配置文件: " + path, {"请检查文件是否存在及读取权限"});
            return false;
        }

        Json::CharReaderBuilder builder;
        std::string errs;
        if (!Json::parseFromStream(builder, ifs, &root, &errs)) {
            printErrors("JSON 解析失败: " + path, {
                errs,
                "请检查 JSON 语法（缺少逗号、引号不匹配、尾部逗号等）"
            });
            return false;
        }

        if (!root.isObject()) {
            printErrors("JSON 格式错误: " + path, {"配置文件根节点必须是 JSON 对象"});
            return false;
        }

        return true;
    }

    // ─── 配置验证 ──────────────────────────────────────────────

    static bool validateConfig(const Json::Value& root, const std::string& path) {
        std::vector<std::string> errors;
        std::vector<std::string> warnings;

        validateListeners(root, errors);
        validateDbClients(root, errors, warnings);
        validateRedisClients(root, errors, warnings);
        validateJwt(root, errors, warnings);

        // 先输出警告（不阻断启动）
        if (!warnings.empty()) {
            printWarnings("配置警告 (" + path + ")", warnings);
        }

        // 有错误则中断启动
        if (!errors.empty()) {
            printErrors("配置验证失败: " + path, errors);
            return false;
        }

        return true;
    }

    static void validateListeners(const Json::Value& root, std::vector<std::string>& errors) {
        if (!root.isMember("listeners") || !root["listeners"].isArray() || root["listeners"].empty()) {
            errors.emplace_back("[listeners] 缺少监听配置，需要至少一个监听地址");
            return;
        }

        for (Json::ArrayIndex i = 0; i < root["listeners"].size(); ++i) {
            const auto& item = root["listeners"][i];
            auto prefix = "[listeners[" + std::to_string(i) + "]] ";

            if (!item.isMember("address") || !item["address"].isString() ||
                item["address"].asString().empty()) {
                errors.push_back(prefix + "缺少 address 字段");
            }

            validatePort(item, prefix, errors);
        }
    }

    static void validateDbClients(const Json::Value& root,
                                  std::vector<std::string>& errors,
                                  std::vector<std::string>& warnings) {
        if (!root.isMember("db_clients") || !root["db_clients"].isArray() ||
            root["db_clients"].empty()) {
            errors.emplace_back("[db_clients] 缺少数据库配置，需要至少一个 PostgreSQL 连接");
            return;
        }

        for (Json::ArrayIndex i = 0; i < root["db_clients"].size(); ++i) {
            const auto& db = root["db_clients"][i];
            auto prefix = "[db_clients[" + std::to_string(i) + "]] ";

            // 必填字符串字段
            for (const char* field : {"name", "rdbms", "host", "user", "dbname"}) {
                if (!db.isMember(field) || !db[field].isString() ||
                    db[field].asString().empty()) {
                    errors.push_back(prefix + "缺少必填字段: " + field);
                }
            }

            validatePort(db, prefix, errors);

            // 密码字段：必须存在，检测占位符
            if (!db.isMember("passwd") || !db["passwd"].isString()) {
                errors.push_back(prefix + "缺少 passwd 字段");
            } else if (isPlaceholder(db["passwd"].asString())) {
                warnings.push_back(prefix + "passwd 看起来是占位符，请填入实际密码");
            }
        }
    }

    static void validateRedisClients(const Json::Value& root,
                                     std::vector<std::string>& errors,
                                     std::vector<std::string>& warnings) {
        if (!root.isMember("redis_clients") || !root["redis_clients"].isArray() ||
            root["redis_clients"].empty()) {
            errors.emplace_back("[redis_clients] 缺少 Redis 配置，需要至少一个 Redis 连接");
            return;
        }

        for (Json::ArrayIndex i = 0; i < root["redis_clients"].size(); ++i) {
            const auto& redis = root["redis_clients"][i];
            auto prefix = "[redis_clients[" + std::to_string(i) + "]] ";

            if (!redis.isMember("host") || !redis["host"].isString() ||
                redis["host"].asString().empty()) {
                errors.push_back(prefix + "缺少 host 字段");
            }

            validatePort(redis, prefix, errors);

            // Redis 密码可以为空（本地开发），但检测占位符
            if (redis.isMember("passwd") && redis["passwd"].isString() &&
                isPlaceholder(redis["passwd"].asString())) {
                warnings.push_back(prefix + "passwd 看起来是占位符，请填入实际密码");
            }
        }
    }

    static void validateJwt(const Json::Value& root,
                            std::vector<std::string>& errors,
                            std::vector<std::string>& warnings) {
        if (!root.isMember("custom_config") || !root["custom_config"].isObject()) {
            errors.emplace_back("[custom_config] 缺少自定义配置节");
            return;
        }

        const auto& custom = root["custom_config"];
        if (!custom.isMember("jwt") || !custom["jwt"].isObject()) {
            errors.emplace_back("[custom_config.jwt] 缺少 JWT 配置");
            return;
        }

        const auto& jwt = custom["jwt"];

        // access token secret
        if (!jwt.isMember("secret") || !jwt["secret"].isString() ||
            jwt["secret"].asString().empty()) {
            errors.emplace_back("[jwt] 缺少 secret 字段");
        } else {
            const auto& secret = jwt["secret"].asString();
            if (isPlaceholder(secret)) {
                warnings.emplace_back("[jwt] secret 看起来是占位符，请设置安全的密钥");
            } else if (secret == "secret") {
                warnings.emplace_back("[jwt] secret 不安全（'secret'），生产环境请使用随机字符串");
            } else if (secret.size() < 16) {
                warnings.push_back("[jwt] secret 长度过短（" +
                    std::to_string(secret.size()) + " 字符），建议至少 32 个字符");
            }
        }

        // access token expiry
        if (!jwt.isMember("access_token_expires_in") ||
            !jwt["access_token_expires_in"].isNumeric()) {
            errors.emplace_back("[jwt] 缺少 access_token_expires_in 字段");
        } else if (jwt["access_token_expires_in"].asInt() <= 0) {
            errors.emplace_back("[jwt] access_token_expires_in 必须大于 0");
        }

        // refresh token secret
        if (!jwt.isMember("refresh_token_secret") || !jwt["refresh_token_secret"].isString() ||
            jwt["refresh_token_secret"].asString().empty()) {
            errors.emplace_back("[jwt] 缺少 refresh_token_secret 字段");
        } else if (isPlaceholder(jwt["refresh_token_secret"].asString())) {
            warnings.emplace_back("[jwt] refresh_token_secret 看起来是占位符");
        }

        // refresh token expiry
        if (!jwt.isMember("refresh_token_expires_in") ||
            !jwt["refresh_token_expires_in"].isNumeric()) {
            errors.emplace_back("[jwt] 缺少 refresh_token_expires_in 字段");
        } else if (jwt["refresh_token_expires_in"].asInt() <= 0) {
            errors.emplace_back("[jwt] refresh_token_expires_in 必须大于 0");
        }
    }

    // ─── 配置应用 ──────────────────────────────────────────────

    static void applyConfig(const Json::Value& root) {
        if (root.isMember("db_clients") && root["db_clients"].isArray() &&
            !root["db_clients"].empty()) {
            AppDbConfig::useFast() = root["db_clients"][0].get("is_fast", false).asBool();
        }
        if (root.isMember("redis_clients") && root["redis_clients"].isArray() &&
            !root["redis_clients"].empty()) {
            AppRedisConfig::useFast() = root["redis_clients"][0].get("is_fast", false).asBool();
        }
        if (root.isMember("app") && root["app"].isMember("number_of_threads")) {
            numberOfThreads_ = static_cast<size_t>(root["app"]["number_of_threads"].asUInt());
        }
    }

    // ─── 工具方法 ──────────────────────────────────────────────

    static void validatePort(const Json::Value& obj, const std::string& prefix,
                             std::vector<std::string>& errors) {
        if (!obj.isMember("port") || !obj["port"].isNumeric()) {
            errors.push_back(prefix + "缺少 port 字段");
        } else {
            int port = obj["port"].asInt();
            if (port < 1 || port > 65535) {
                errors.push_back(prefix + "port 值无效: " +
                    std::to_string(port) + "（有效范围: 1-65535）");
            }
        }
    }

    static bool isPlaceholder(const std::string& value) {
        if (value.empty()) return false;
        if (value.starts_with("YOUR_") || value.starts_with("your_")) return true;
        if (value.find("CHANGE_ME") != std::string::npos) return true;
        if (value.find("TODO") != std::string::npos) return true;
        if (value == "password" || value == "PASSWORD") return true;
        return false;
    }

    static void printErrors(const std::string& title, const std::vector<std::string>& messages) {
        std::string border(60, '=');
        std::cerr << "\n" << border << "\n";
        std::cerr << " [ERROR] " << title << "\n";
        std::cerr << std::string(60, '-') << "\n";
        for (const auto& msg : messages) {
            std::cerr << "  " << msg << "\n";
            LOG_ERROR << "[Config] " << msg;
        }
        std::cerr << border << "\n" << std::endl;
    }

    static void printWarnings(const std::string& title, const std::vector<std::string>& messages) {
        std::cerr << "\n" << std::string(60, '-') << "\n";
        std::cerr << " [WARN] " << title << "\n";
        for (const auto& msg : messages) {
            std::cerr << "  " << msg << "\n";
            LOG_WARN << "[Config] " << msg;
        }
        std::cerr << std::string(60, '-') << "\n" << std::endl;
    }
};
