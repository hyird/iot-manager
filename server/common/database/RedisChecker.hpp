#pragma once

#include "RedisService.hpp"

namespace fs = std::filesystem;

/**
 * @brief Redis 可用性检查工具
 *
 * 读取配置文件判断 Redis 是否已配置，
 * 并初始化 AppRedisConfig 的 fast 模式设置
 */
class RedisChecker {
public:
    /**
     * @brief 检查 Redis 是否可用
     * @return true 如果配置文件中存在 Redis 配置
     */
    static bool checkAvailable() {
        std::vector<std::string> configPaths = {
            "./config/config.json",
            "../../config/config.json",
            "../config/config.json",
            "config.json"
        };

        for (const auto& path : configPaths) {
            if (fs::exists(path)) {
                try {
                    std::ifstream ifs(path);
                    if (ifs) {
                        Json::Value root;
                        Json::CharReaderBuilder builder;
                        std::string errs;
                        if (Json::parseFromStream(builder, ifs, &root, &errs)) {
                            bool hasRedis = root.isMember("redis_clients") &&
                                           root["redis_clients"].isArray() &&
                                           !root["redis_clients"].empty();

                            if (hasRedis) {
                                const auto& redisConfig = root["redis_clients"][0];
                                bool isFast = redisConfig.isMember("is_fast") &&
                                             redisConfig["is_fast"].asBool();
                                AppRedisConfig::useFast() = isFast;
                                LOG_INFO << "Redis is configured (fast mode: "
                                         << (isFast ? "true" : "false") << ")";
                                return true;
                            } else {
                                LOG_INFO << "Redis is not configured in config file";
                                return false;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_WARN << "Failed to check Redis config: " << e.what();
                }
            }
        }

        return false;
    }
};
