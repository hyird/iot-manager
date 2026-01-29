#pragma once

#include <drogon/HttpAppFramework.h>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include "common/database/DatabaseService.hpp"

using namespace drogon;
namespace fs = std::filesystem;

/**
 * @brief 配置管理器 - 负责加载和管理应用配置
 */
class ConfigManager {
public:
    /**
     * @brief 加载配置文件
     * @return 是否成功加载
     */
    static bool load() {
        std::vector<std::string> configPaths = {
            "./config/config.json",
            "../../config/config.json",
            "../config/config.json",
            "config.json"
        };

        for (const auto& path : configPaths) {
            if (fs::exists(path)) {
                try {
                    // 先读取配置文件获取 is_fast
                    std::ifstream ifs(path);
                    if (ifs) {
                        Json::Value root;
                        Json::CharReaderBuilder builder;
                        std::string errs;
                        if (Json::parseFromStream(builder, ifs, &root, &errs)) {
                            if (root.isMember("db_clients") && root["db_clients"].isArray() &&
                                !root["db_clients"].empty()) {
                                AppDbConfig::useFast() = root["db_clients"][0].get("is_fast", false).asBool();
                            }
                            // 解析线程数配置
                            if (root.isMember("app") && root["app"].isMember("number_of_threads")) {
                                numberOfThreads_ = static_cast<size_t>(root["app"]["number_of_threads"].asUInt());
                            }
                        }
                    }

                    app().loadConfigFile(path);
                    LOG_INFO << "Config loaded from: " << path;
                    return true;
                } catch (const std::exception& e) {
                    LOG_WARN << "Failed to load " << path << ": " << e.what();
                }
            }
        }
        return false;
    }

    /**
     * @brief 获取日志级别配置
     */
    static std::string getLogLevel() {
        auto& config = app().getCustomConfig();
        return config.get("log_level", "INFO").asString();
    }

    /**
     * @brief 获取是否启用控制台日志
     */
    static bool isConsoleLogEnabled() {
        auto& config = app().getCustomConfig();
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
    // 在 load() 中解析的线程数配置
    inline static size_t numberOfThreads_ = 0;
};
