#pragma once

#include "RedisService.hpp"

/**
 * @brief Redis 可用性检查工具
 *
 * 基于已加载的 Drogon 配置检查 Redis 客户端是否可获取
 */
class RedisChecker {
public:
    /**
     * @brief 检查 Redis 是否可用
     * @return true 如果可获取到默认 Redis 客户端
     */
    static bool checkAvailable() {
        try {
            auto client = AppRedisConfig::useFast()
                ? drogon::app().getFastRedisClient("default")
                : drogon::app().getRedisClient("default");
            if (!client) {
                LOG_WARN << "Redis client 'default' is not configured";
                return false;
            }
            LOG_INFO << "Redis client is configured (fast mode: "
                     << (AppRedisConfig::useFast() ? "true" : "false") << ")";
            return true;
        } catch (const std::exception& e) {
            LOG_WARN << "Failed to acquire Redis client: " << e.what();
            return false;
        }
    }
};
