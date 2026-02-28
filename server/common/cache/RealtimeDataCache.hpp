#pragma once

#include "common/database/DatabaseService.hpp"
#include "common/database/RedisService.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/JsonHelper.hpp"
#include "common/utils/SqlHelper.hpp"

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
     * @brief 更新设备的某个功能码数据（fire-and-forget，非协程上下文使用）
     */
    void update(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        drogon::async_run([this, deviceId, funcCode, data, reportTime]() -> Task<void> {
            try {
                co_await updateRedis(deviceId, funcCode, data, reportTime);
            } catch (const std::exception& e) {
                LOG_ERROR << "[RealtimeDataCache] update failed for device " << deviceId << ": " << e.what();
            }
        });
    }

    /**
     * @brief 更新设备的某个功能码数据（可 co_await，用于需要确保写入完成的场景）
     */
    Task<void> updateAsync(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        co_await updateRedis(deviceId, funcCode, data, reportTime);
    }

    /**
     * @brief 合并更新设备的某个功能码数据（fire-and-forget）
     *
     * 与 update() 的区别：不整体覆盖，而是将新数据的寄存器字段合并到现有条目。
     * 用于 Modbus 多 ReadGroup 场景：不同寄存器类型的响应相互不覆盖。
     */
    void mergeUpdate(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        drogon::async_run([this, deviceId, funcCode, data, reportTime]() -> Task<void> {
            try {
                co_await mergeUpdateRedis(deviceId, funcCode, data, reportTime);
            } catch (const std::exception& e) {
                LOG_ERROR << "[RealtimeDataCache] mergeUpdate failed for device " << deviceId << ": " << e.what();
            }
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
        // 协程安全：确保同一时刻仅一个初始化任务执行，失败后允许重试
        while (true) {
            if (initialized_.load(std::memory_order_acquire)) co_return;

            bool expected = false;
            if (initializing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                break;  // 当前协程成为初始化执行者
            }

            co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(50));
        }

        struct InitFlagGuard {
            std::atomic<bool>& flag;
            ~InitFlagGuard() {
                flag.store(false, std::memory_order_release);
            }
        } initGuard{initializing_};

        if (deviceIds.empty()) {
            initialized_.store(true, std::memory_order_release);
            co_return;
        }

        // 构建参数化 IN 子句
        auto [placeholders, params] = SqlHelper::buildParameterizedIn(deviceIds);
        params.push_back(std::to_string(Constants::DEVICE_DATA_LOOKBACK_DAYS));

        // 查询每个设备每个功能码的最新数据
        DatabaseService dbService;
        std::string sql =
            "SELECT device_id, data, report_time, data->>'funcCode' as func_code "
            "FROM ( "
            "  SELECT DISTINCT ON (device_id, data->>'funcCode') "
            "         device_id, data, report_time "
            "  FROM device_data "
            "  WHERE device_id IN (" + placeholders + ") "
            "    AND report_time >= NOW() - make_interval(days => ?::int) "
            "  ORDER BY device_id, data->>'funcCode', report_time DESC NULLS LAST "
            ") sub";

        auto result = co_await dbService.execSqlCoro(sql, params);

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

        initialized_.store(true, std::memory_order_release);
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
            try {
                co_await invalidateRedis(deviceId);
            } catch (const std::exception& e) {
                LOG_ERROR << "[RealtimeDataCache] invalidate failed for device " << deviceId << ": " << e.what();
            }
        });
    }

    /**
     * @brief 清除所有缓存
     */
    void invalidateAll() {
        initialized_.store(false, std::memory_order_release);
        initializing_.store(false, std::memory_order_release);
        drogon::async_run([this]() -> Task<void> {
            try {
                co_await invalidateAllRedis();
            } catch (const std::exception& e) {
                LOG_ERROR << "[RealtimeDataCache] invalidateAll failed: " << e.what();
            }
        });
    }

private:
    RealtimeDataCache() = default;

    // Redis 键前缀
    static constexpr const char* REDIS_KEY_PREFIX = "realtime:device:";
    static constexpr const char* REDIS_LATEST_PREFIX = "realtime:latest:";
    static constexpr int REDIS_TTL = Constants::REDIS_TTL_REALTIME_DATA;

    /** 预编译 Lua 脚本：整体替换（TTL 在首次调用时烘焙进字符串，避免每次拼接） */
    static const std::string& getUpdateScript() {
        static const std::string script = []() {
            auto ttl = std::to_string(REDIS_TTL);
            return std::string(
                "redis.call('HSET',KEYS[1],ARGV[1],ARGV[2]) "
                "redis.call('EXPIRE',KEYS[1],") + ttl + ") "
                "local cur=redis.call('GET',KEYS[2]) "
                "if not cur or ARGV[3]>cur then redis.call('SETEX',KEYS[2]," + ttl + ",ARGV[3]) end "
                "return 1";
        }();
        return script;
    }

    /**
     * @brief 预编译 Lua 脚本：合并更新
     *
     * 将新数据的 data.data 字段逐一合并到现有条目，而不是整体覆盖。
     * 用于 Modbus 多次 ReadGroup 响应：每次只更新本批寄存器，不影响其他类型的值。
     * KEYS: [1]=dataKey [2]=latestKey
     * ARGV: [1]=funcCode [2]=newJsonStr [3]=reportTime
     */
    static const std::string& getMergeUpdateScript() {
        static const std::string script = []() {
            auto ttl = std::to_string(REDIS_TTL);
            return std::string(
                "local ex=redis.call('HGET',KEYS[1],ARGV[1]) "
                "if ex then "
                "  local s,old=pcall(cjson.decode,ex) "
                "  local s2,nw=pcall(cjson.decode,ARGV[2]) "
                "  if s and s2 and type(old)=='table' and type(nw)=='table' "
                "     and type(old['data'])=='table' and type(nw['data'])=='table' "
                "     and type(old['data']['data'])=='table' "
                "     and type(nw['data']['data'])=='table' then "
                "    for k,v in pairs(nw['data']['data']) do old['data']['data'][k]=v end "
                "    old['reportTime']=ARGV[3] "
                "    redis.call('HSET',KEYS[1],ARGV[1],cjson.encode(old)) "
                "  else "
                "    redis.call('HSET',KEYS[1],ARGV[1],ARGV[2]) "
                "  end "
                "else "
                "  redis.call('HSET',KEYS[1],ARGV[1],ARGV[2]) "
                "end "
                "redis.call('EXPIRE',KEYS[1],") + ttl + ") "
                "local cur=redis.call('GET',KEYS[2]) "
                "if not cur or ARGV[3]>cur then redis.call('SETEX',KEYS[2]," + ttl + ",ARGV[3]) end "
                "return 1";
        }();
        return script;
    }

    std::atomic<bool> initialized_{false};
    std::atomic<bool> initializing_{false};  // 防止并发初始化（TOCTOU）

    // ==================== Redis 缓存操作 ====================

    /**
     * @brief 更新单个设备的实时数据（Lua 脚本，单次 Redis 往返）
     * 合并 HSET + EXPIRE + 条件 SETEX 为一次调用
     */
    Task<void> updateRedis(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        Json::Value cacheData;
        cacheData["data"] = data;
        cacheData["reportTime"] = reportTime;
        std::string jsonStr = JsonHelper::serialize(cacheData);

        std::string dataKey = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);
        std::string latestKey = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);

        // HSET + EXPIRE + 条件 SETEX（仅当新时间更新时覆盖）
        // KEYS: [1]=dataKey [2]=latestKey
        // ARGV: [1]=funcCode [2]=jsonStr [3]=reportTime
        // 使用预编译脚本（TTL 已烘焙，避免每次拼接字符串）
        co_await client->execCommandCoro(
            "EVAL %s 2 %s %s %s %s %s",
            getUpdateScript().c_str(),
            dataKey.c_str(), latestKey.c_str(),
            funcCode.c_str(), jsonStr.c_str(), reportTime.c_str()
        );
    }

    /**
     * @brief 合并更新单个设备的实时数据（Lua 脚本，原子 merge，单次 Redis 往返）
     * 将新数据的 data.data 字段逐一写入，保留已有的其他寄存器字段
     */
    Task<void> mergeUpdateRedis(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        RedisService redis;
        auto client = redis.getClient();
        if (!client) {
            throw std::runtime_error("Redis client not available");
        }

        Json::Value cacheData;
        cacheData["data"] = data;
        cacheData["reportTime"] = reportTime;
        std::string jsonStr = JsonHelper::serialize(cacheData);

        std::string dataKey = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);
        std::string latestKey = std::string(REDIS_LATEST_PREFIX) + std::to_string(deviceId);

        co_await client->execCommandCoro(
            "EVAL %s 2 %s %s %s %s %s",
            getMergeUpdateScript().c_str(),
            dataKey.c_str(), latestKey.c_str(),
            funcCode.c_str(), jsonStr.c_str(), reportTime.c_str()
        );
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

        // 使用 Pipeline：先发送所有命令，最后一起获取结果
        // Drogon Redis 客户端会自动批量发送多个命令

        // 写入设备实时数据
        for (const auto& [deviceId, deviceData] : dataMap) {
            std::string key = std::string(REDIS_KEY_PREFIX) + std::to_string(deviceId);

            for (const auto& [funcCode, funcData] : deviceData) {
                Json::Value cacheData;
                cacheData["data"] = funcData.data;
                cacheData["reportTime"] = funcData.reportTime;
                std::string jsonStr = JsonHelper::serialize(cacheData);

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
     * @brief 批量获取设备实时数据（Lua 脚本，减少 Redis 往返）
     *
     * 使用 EVAL + Lua 在 Redis 侧批量执行 HGETALL，
     * 每批 50 个设备，将 N 次往返减少为 ceil(N/50) 次。
     */
    Task<std::map<int, DeviceRealtimeData>> getBatchFromRedis(const std::vector<int>& deviceIds) {
        std::map<int, DeviceRealtimeData> result;
        if (deviceIds.empty()) co_return result;

        RedisService redis;
        auto client = redis.getClient();
        if (!client) throw std::runtime_error("Redis client not available");

        Json::CharReaderBuilder readerBuilder;
        constexpr size_t SUB_BATCH = 50;
        bool needFallback = false;
        size_t fallbackStart = 0, fallbackEnd = 0;

        for (size_t start = 0; start < deviceIds.size(); start += SUB_BATCH) {
            size_t end = std::min(start + SUB_BATCH, deviceIds.size());

            // 构建 EVAL 命令：
            // EVAL script numkeys key1 key2 ... deviceId1 deviceId2 ...
            // Lua 返回扁平数组: [deviceId, fieldCount, f1, v1, f2, v2, ..., deviceId, ...]
            // 注意：Lua 脚本必须通过 %s 传递，否则 hiredis 会按空格拆分脚本内容
            static const std::string luaScript =
                "local r={} "
                "for i=1,#KEYS do "
                "  local d=redis.call('HGETALL',KEYS[i]) "
                "  r[#r+1]=ARGV[i] r[#r+1]=tostring(#d) "
                "  for _,v in ipairs(d) do r[#r+1]=v end "
                "end "
                "return r";

            std::ostringstream fmt;
            fmt << "EVAL %s " << (end - start);  // numkeys
            // KEYS: Redis 键
            for (size_t i = start; i < end; ++i) {
                fmt << " " << REDIS_KEY_PREFIX << deviceIds[i];
            }
            // ARGV: deviceId（用于结果映射）
            for (size_t i = start; i < end; ++i) {
                fmt << " " << deviceIds[i];
            }

            try {
                auto evalResult = co_await client->execCommandCoro(fmt.str().c_str(), luaScript.c_str());

                if (evalResult.isNil() || evalResult.type() != drogon::nosql::RedisResultType::kArray) continue;

                auto arr = evalResult.asArray();
                size_t idx = 0;

                while (idx + 1 < arr.size()) {
                    int deviceId = std::stoi(arr[idx].asString());
                    int pairCount = std::stoi(arr[idx + 1].asString()) / 2;  // Lua #d 返回元素总数，÷2 得 field-value 对数
                    idx += 2;

                    DeviceRealtimeData deviceData;
                    for (int f = 0; f < pairCount && (idx + 1) < arr.size(); ++f) {
                        std::string funcCode = arr[idx].asString();
                        std::string jsonStr = arr[idx + 1].asString();
                        idx += 2;

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
                        result[deviceId] = std::move(deviceData);
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN << "[RealtimeDataCache] Lua batch HGETALL failed: " << e.what()
                         << ", falling back to individual reads";
                // MSVC 不支持 catch 中 co_await，记录需要回退的范围
                fallbackStart = start;
                fallbackEnd = end;
                needFallback = true;
            }

            // catch 外执行回退逻辑
            if (needFallback) {
                needFallback = false;
                for (size_t i = fallbackStart; i < fallbackEnd; ++i) {
                    auto deviceData = co_await getFromRedis(deviceIds[i]);
                    if (deviceData) {
                        result[deviceIds[i]] = std::move(*deviceData);
                    }
                }
            }
        }

        co_return result;
    }

    /**
     * @brief 批量获取设备最新上报时间（MGET，单次 Redis 往返）
     */
    Task<std::map<int, std::string>> getLatestReportTimesFromRedis(const std::vector<int>& deviceIds) {
        std::map<int, std::string> result;
        if (deviceIds.empty()) co_return result;

        RedisService redis;
        auto client = redis.getClient();
        if (!client) throw std::runtime_error("Redis client not available");

        bool mgetFallback = false;

        // 构建 MGET 命令：MGET key1 key2 key3 ...
        std::ostringstream cmd;
        cmd << "MGET";
        for (int id : deviceIds) {
            cmd << " " << REDIS_LATEST_PREFIX << id;
        }

        try {
            auto mgetResult = co_await client->execCommandCoro(cmd.str().c_str());

            if (!mgetResult.isNil() && mgetResult.type() == drogon::nosql::RedisResultType::kArray) {
                auto arr = mgetResult.asArray();
                for (size_t i = 0; i < arr.size() && i < deviceIds.size(); ++i) {
                    if (!arr[i].isNil()) {
                        std::string time = arr[i].asString();
                        if (!time.empty()) {
                            result[deviceIds[i]] = time;
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN << "[RealtimeDataCache] MGET failed: " << e.what()
                     << ", falling back to individual reads";
            mgetFallback = true;
        }
        if (mgetFallback) {
            for (int deviceId : deviceIds) {
                std::string time = co_await getLatestReportTimeFromRedis(deviceId);
                if (!time.empty()) {
                    result[deviceId] = time;
                }
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
