#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/database/RedisService.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief 实时数据缓存服务（纯 Redis 版本）
 *
 * 缓存每个设备每个功能码的最新数据
 * - 仅使用 Redis 缓存，Redis 必须可用
 * - 首次访问时从数据库加载
 * - 后续通过报文解析更新缓存
 * - 实时查询直接从缓存返回，无需查询数据库
 */
class RealtimeDataCache {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    // 单个功能码的实时数据
    struct FuncData {
        Json::Value data;           // 完整的 JSONB 数据
        std::string reportTime;     // 上报时间
    };

    // 设备的实时数据（funcCode -> FuncData）
    using DeviceRealtimeData = std::map<std::string, FuncData>;

    static RealtimeDataCache& instance() {
        static RealtimeDataCache instance;
        return instance;
    }

    /**
     * @brief 更新设备的某个功能码数据（报文解析后调用）
     */
    void update(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        drogon::async_run([this, deviceId, funcCode, data, reportTime]() -> Task<void> {
            co_await updateRedis(deviceId, funcCode, data, reportTime);
        });
    }

    /**
     * @brief 获取设备的所有实时数据
     */
    Task<std::optional<DeviceRealtimeData>> get(int deviceId) {
        co_return co_await getFromRedis(deviceId);
    }

    /**
     * @brief 获取设备最新上报时间
     */
    Task<std::string> getLatestReportTime(int deviceId) {
        co_return co_await getLatestReportTimeFromRedis(deviceId);
    }

    /**
     * @brief 批量获取设备实时数据
     */
    Task<std::map<int, DeviceRealtimeData>> getBatch(const std::vector<int>& deviceIds) {
        co_return co_await getBatchFromRedis(deviceIds);
    }

    /**
     * @brief 批量获取设备最新上报时间
     */
    Task<std::map<int, std::string>> getLatestReportTimes(const std::vector<int>& deviceIds) {
        co_return co_await getLatestReportTimesFromRedis(deviceIds);
    }

    /**
     * @brief 检查缓存是否已初始化（是否有数据）
     */
    bool isInitialized() const {
        return initialized_.load();
    }

    /**
     * @brief 从数据库初始化缓存（首次访问时调用）
     */
    Task<void> initializeFromDb(const std::vector<int>& deviceIds) {
        if (deviceIds.empty()) co_return;

        // 构建 ID 列表
        std::ostringstream idListBuilder;
        for (size_t i = 0; i < deviceIds.size(); ++i) {
            if (i > 0) idListBuilder << ",";
            idListBuilder << deviceIds[i];
        }
        std::string idList = idListBuilder.str();

        // 查询每个设备每个功能码的最新数据
        DatabaseService dbService;
        std::string sql = R"(
            SELECT device_id, data, report_time, data->>'funcCode' as func_code
            FROM (
                SELECT DISTINCT ON (device_id, data->>'funcCode')
                       device_id, data, report_time
                FROM device_data
                WHERE device_id IN ()" + idList + R"()
                  AND report_time >= NOW() - INTERVAL ')" + std::to_string(Constants::DEVICE_DATA_LOOKBACK_DAYS) + R"( days'
                ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST
            ) sub
        )";

        auto result = co_await dbService.execSqlCoro(sql);

        Json::CharReaderBuilder readerBuilder;

        // 临时存储解析后的数据
        std::map<int, DeviceRealtimeData> tempCache;
        std::map<int, std::string> tempLatestTime;

        for (const auto& row : result) {
            int deviceId = FieldHelper::getInt(row["device_id"]);
            std::string funcCode = FieldHelper::getString(row["func_code"], "");
            std::string reportTime = FieldHelper::getString(row["report_time"], "");
            std::string dataStr = FieldHelper::getString(row["data"], "");

            Json::Value dataJson;
            std::string errs;
            std::istringstream iss(dataStr);
            if (Json::parseFromStream(readerBuilder, iss, &dataJson, &errs)) {
                tempCache[deviceId][funcCode] = {dataJson, reportTime};

                // 更新设备最新上报时间
                auto& latestTime = tempLatestTime[deviceId];
                if (latestTime.empty() || reportTime > latestTime) {
                    latestTime = reportTime;
                }
            }
        }

        // 批量写入 Redis（使用 Pipeline 优化）
        co_await batchUpdateRedis(tempCache, tempLatestTime);

        initialized_ = true;
        LOG_DEBUG << "[RealtimeDataCache] Initialized from DB for " << deviceIds.size() << " devices";
    }

    /**
     * @brief 清除指定设备的最新上报时间（标记为离线）
     * 保留实时数据不变，仅清除 latest 键使设备显示为离线
     */
    void clearLatestTime(int deviceId) {
        // Redis 操作必须在 Drogon IO 线程执行（可能从 TcpIoPool 线程调用）
        drogon::app().getIOLoop(0)->queueInLoop([deviceId]() {
            drogon::async_run([deviceId]() -> Task<void> {
                try {
                    RedisService redis;
                    auto client = redis.getClient();
                    if (!client) co_return;

                    std::string key = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);
                    co_await client->execCommandCoro("DEL %s", key.c_str());
                } catch (const std::exception& e) {
                    LOG_ERROR << "[RealtimeDataCache] clearLatestTime failed for device " << deviceId << ": " << e.what();
                }
            });
        });
    }

    /**
     * @brief 清除指定设备的缓存
     */
    void invalidate(int deviceId) {
        drogon::async_run([this, deviceId]() -> Task<void> {
            co_await invalidateRedis(deviceId);
        });
    }

    /**
     * @brief 清除所有缓存
     */
    void invalidateAll() {
        initialized_ = false;
        drogon::async_run([this]() -> Task<void> {
            co_await invalidateAllRedis();
        });
    }

private:
    RealtimeDataCache() = default;

    // Redis 键前缀
    static constexpr const char* REDIS_KEY_PREFIX = "realtime:device:";
    static constexpr const char* REDIS_LATEST_PREFIX = "realtime:latest:";
    static constexpr int REDIS_TTL = Constants::REDIS_TTL_REALTIME_DATA;

    std::atomic<bool> initialized_{false};

    // ==================== Redis 缓存操作 ====================

    Task<void> updateRedis(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        // 构建存储的 JSON
        Json::Value cacheData;
        cacheData["data"] = data;
        cacheData["reportTime"] = reportTime;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string jsonStr = Json::writeString(builder, cacheData);

        // 使用 HSET 存储
        std::string key = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);
        co_await client->execCommandCoro("HSET %s %s %s",
            key.c_str(), funcCode.c_str(), jsonStr.c_str());
        co_await client->execCommandCoro("EXPIRE %s %d", key.c_str(), REDIS_TTL);

        // 更新最新上报时间
        co_await setLatestReportTimeToRedis(deviceId, reportTime);
    }

    /**
     * @brief 批量写入 Redis（Pipeline 优化）
     * 将所有 HSET 和 EXPIRE 命令合并到一次网络往返中
     */
    Task<void> batchUpdateRedis(
        const std::map<int, DeviceRealtimeData>& dataMap,
        const std::map<int, std::string>& latestTimeMap
    ) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";

        // 使用 Pipeline：先发送所有命令，最后一起获取结果
        // Drogon Redis 客户端会自动批量发送多个命令

        // 写入设备实时数据
        for (const auto& [deviceId, deviceData] : dataMap) {
            std::string key = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);

            for (const auto& [funcCode, funcData] : deviceData) {
                Json::Value cacheData;
                cacheData["data"] = funcData.data;
                cacheData["reportTime"] = funcData.reportTime;
                std::string jsonStr = Json::writeString(builder, cacheData);

                co_await client->execCommandCoro("HSET %s %s %s",
                    key.c_str(), funcCode.c_str(), jsonStr.c_str());
            }

            // 设置过期时间
            co_await client->execCommandCoro("EXPIRE %s %d", key.c_str(), REDIS_TTL);
        }

        // 批量写入最新上报时间
        for (const auto& [deviceId, latestTime] : latestTimeMap) {
            std::string key = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);
            co_await client->execCommandCoro("SETEX %s %d %s",
                key.c_str(), REDIS_TTL, latestTime.c_str());
        }
    }

    Task<void> setLatestReportTimeToRedis(int deviceId, const std::string& reportTime) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        std::string key = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);

        // 获取当前值进行比较
        auto current = co_await client->execCommandCoro("GET %s", key.c_str());
        if (current.isNil() || reportTime > current.asString()) {
            co_await client->execCommandCoro("SETEX %s %d %s",
                key.c_str(), REDIS_TTL, reportTime.c_str());
        }
    }

    Task<std::optional<DeviceRealtimeData>> getFromRedis(int deviceId) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        std::string key = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);
        auto result = co_await client->execCommandCoro("HGETALL %s", key.c_str());

        if (result.isNil() || result.asArray().empty()) {
            co_return std::nullopt;
        }

        DeviceRealtimeData deviceData;
        auto arr = result.asArray();

        Json::CharReaderBuilder readerBuilder;

        // HGETALL 返回 [field1, value1, field2, value2, ...]
        for (size_t i = 0; i + 1 < arr.size(); i += 2) {
            std::string funcCode = arr[i].asString();
            std::string jsonStr = arr[i + 1].asString();

            Json::Value cacheData;
            std::string errs;
            std::istringstream iss(jsonStr);
            if (Json::parseFromStream(readerBuilder, iss, &cacheData, &errs)) {
                FuncData fd;
                fd.data = cacheData["data"];
                fd.reportTime = cacheData["reportTime"].asString();
                deviceData[funcCode] = std::move(fd);
            }
        }

        if (!deviceData.empty()) {
            co_return deviceData;
        }

        co_return std::nullopt;
    }

    Task<std::string> getLatestReportTimeFromRedis(int deviceId) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        std::string key = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);
        auto result = co_await client->execCommandCoro("GET %s", key.c_str());

        if (!result.isNil()) {
            co_return result.asString();
        }

        co_return "";
    }

    /**
     * @brief 批量获取设备实时数据
     * 逐个获取每个设备的 Hash 数据
     */
    Task<std::map<int, DeviceRealtimeData>> getBatchFromRedis(const std::vector<int>& deviceIds) {
        std::map<int, DeviceRealtimeData> result;

        if (deviceIds.empty()) {
            co_return result;
        }

        // 逐个获取每个设备的数据
        for (int deviceId : deviceIds) {
            auto deviceData = co_await getFromRedis(deviceId);
            if (deviceData) {
                result[deviceId] = std::move(*deviceData);
            }
        }

        co_return result;
    }

    /**
     * @brief 批量获取设备最新上报时间
     * 逐个获取每个设备的上报时间
     */
    Task<std::map<int, std::string>> getLatestReportTimesFromRedis(const std::vector<int>& deviceIds) {
        std::map<int, std::string> result;

        if (deviceIds.empty()) {
            co_return result;
        }

        // 逐个获取每个设备的最新上报时间
        for (int deviceId : deviceIds) {
            std::string time = co_await getLatestReportTimeFromRedis(deviceId);
            if (!time.empty()) {
                result[deviceId] = time;
            }
        }

        co_return result;
    }

    Task<void> invalidateRedis(int deviceId) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        std::string dataKey = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);
        std::string latestKey = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);

        co_await client->execCommandCoro("DEL %s %s", dataKey.c_str(), latestKey.c_str());
    }

    Task<void> invalidateAllRedis() {
        RedisService redis;
        co_await redis.delPattern(std::string(REDIS_KEY_PREFIX) + "*");
        co_await redis.delPattern(std::string(REDIS_LATEST_PREFIX) + "*");
    }
};
