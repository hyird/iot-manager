#pragma once

#include "common/utils/JsonHelper.hpp"

/**
 * @brief Redis 配置（由 main.cpp 初始化）
 */
struct AppRedisConfig {
    static bool& useFast() {
        static bool value = false;
        return value;
    }
};

/**
 * @brief Redis 服务类
 * 提供统一的 Redis 访问接口，Redis 必须可用
 */
class RedisService {
public:
    using RedisClientPtr = drogon::nosql::RedisClientPtr;
    template<typename T = void> using Task = drogon::Task<T>;

    RedisService() = default;

    /**
     * @brief 获取 Redis 客户端
     * @throws std::runtime_error 如果无法获取客户端
     */
    RedisClientPtr getClient() const {
        try {
            auto client = AppRedisConfig::useFast()
                ? drogon::app().getFastRedisClient("default")
                : drogon::app().getRedisClient("default");
            if (!client) {
                throw std::runtime_error("Redis client is null");
            }
            return client;
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to get Redis client: ") + e.what());
        }
    }

    /**
     * @brief 探测 Redis 连接
     * @throws std::runtime_error 如果 PING 失败
     */
    Task<void> ping() {
        auto client = getClient();

        auto result = co_await client->execCommandCoro("PING");
        if (result.isNil()) {
            throw std::runtime_error("Redis PING returned nil");
        }

        std::string reply = result.asString();
        if (reply != "PONG" && reply != "pong") {
            throw std::runtime_error("Redis PING unexpected response: " + reply);
        }
    }

    /**
     * @brief 获取字符串值
     */
    Task<std::optional<std::string>> get(const std::string& key) {
        auto client = getClient();

        auto result = co_await client->execCommandCoro("GET %s", key.c_str());
        if (result.isNil()) {
            co_return std::nullopt;
        }
        co_return result.asString();
    }

    /**
     * @brief 设置字符串值（支持 TTL）
     */
    Task<bool> set(const std::string& key, const std::string& value, int ttl = 0) {
        auto client = getClient();

        if (ttl > 0) {
            co_await client->execCommandCoro("SETEX %s %d %s",
                key.c_str(), ttl, value.c_str());
        } else {
            co_await client->execCommandCoro("SET %s %s",
                key.c_str(), value.c_str());
        }
        co_return true;
    }

    /**
     * @brief 获取 JSON 对象
     */
    Task<std::optional<Json::Value>> getJson(const std::string& key) {
        auto str = co_await get(key);
        if (!str) {
            co_return std::nullopt;
        }

        Json::Value json;
        Json::CharReaderBuilder builder;
        std::istringstream stream(*str);
        std::string errs;

        if (Json::parseFromStream(builder, stream, &json, &errs)) {
            co_return json;
        }
        LOG_WARN << "Failed to parse JSON from Redis key '" << key << "': " << errs;
        co_return std::nullopt;
    }

    /**
     * @brief 设置 JSON 对象
     */
    Task<bool> setJson(const std::string& key, const Json::Value& value, int ttl = 0) {
        co_return co_await set(key, JsonHelper::serialize(value), ttl);
    }

    /**
     * @brief 删除键
     */
    Task<bool> del(const std::string& key) {
        auto client = getClient();

        co_await client->execCommandCoro("DEL %s", key.c_str());
        co_return true;
    }

    /**
     * @brief 删除多个键（支持通配符）
     * 使用 SCAN 代替 KEYS 避免阻塞，使用 UNLINK 异步删除提高性能
     */
    Task<int> delPattern(const std::string& pattern) {
        auto client = getClient();
        int deleted = 0;
        std::string cursor = "0";

        do {
            // 使用 SCAN 分批获取匹配的键（每批 100 个）
            auto result = co_await client->execCommandCoro(
                "SCAN %s MATCH %s COUNT 100", cursor.c_str(), pattern.c_str());

            if (result.isNil() || result.asArray().size() < 2) {
                break;
            }

            auto arr = result.asArray();
            cursor = arr[0].asString();
            auto keys = arr[1].asArray();

            if (!keys.empty()) {
                // 批量删除：构建 UNLINK 命令（异步删除，不阻塞）
                std::ostringstream cmd;
                cmd << "UNLINK";
                for (const auto& key : keys) {
                    cmd << " " << key.asString();
                }
                co_await client->execCommandCoro(cmd.str().c_str());
                deleted += static_cast<int>(keys.size());
            }
        } while (cursor != "0");

        co_return deleted;
    }

    /**
     * @brief 检查键是否存在
     */
    Task<bool> exists(const std::string& key) {
        auto client = getClient();

        auto result = co_await client->execCommandCoro("EXISTS %s", key.c_str());
        co_return result.asInteger() > 0;
    }

    /**
     * @brief 设置过期时间
     */
    Task<bool> expire(const std::string& key, int seconds) {
        auto client = getClient();

        auto result = co_await client->execCommandCoro("EXPIRE %s %d",
            key.c_str(), seconds);
        co_return result.asInteger() > 0;
    }

    /**
     * @brief 增加计数器
     */
    Task<int64_t> incr(const std::string& key) {
        auto client = getClient();

        auto result = co_await client->execCommandCoro("INCR %s", key.c_str());
        co_return result.asInteger();
    }

    /**
     * @brief 增加计数器并设置过期时间（用于限流）
     */
    Task<int64_t> incrWithExpire(const std::string& key, int ttl) {
        auto count = co_await incr(key);
        if (count == 1) {
            co_await expire(key, ttl);
        }
        co_return count;
    }
};
