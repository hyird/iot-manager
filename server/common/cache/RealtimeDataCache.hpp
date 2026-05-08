#pragma once

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "common/database/DatabaseService.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/SqlHelper.hpp"

class RealtimeDataCache {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    struct FuncData {
        Json::Value data;
        std::string reportTime;
    };

    using DeviceRealtimeData = std::map<std::string, FuncData>;

    static RealtimeDataCache& instance() {
        static RealtimeDataCache instance;
        return instance;
    }

    void update(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        updateMemory(deviceId, funcCode, data, reportTime);
    }

    Task<void> updateAsync(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        updateMemory(deviceId, funcCode, data, reportTime);
        co_return;
    }

    void mergeUpdate(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        mergeUpdateMemory(deviceId, funcCode, data, reportTime);
    }

    Task<std::optional<DeviceRealtimeData>> get(int deviceId) {
        std::shared_lock lock(mutex_);
        auto it = cache_.find(deviceId);
        if (it == cache_.end() || it->second.empty()) {
            co_return std::nullopt;
        }
        co_return it->second;
    }

    Task<std::string> getLatestReportTime(int deviceId) {
        std::shared_lock lock(mutex_);
        auto it = latestReportTimes_.find(deviceId);
        co_return it != latestReportTimes_.end() ? it->second : "";
    }

    Task<std::map<int, DeviceRealtimeData>> getBatch(const std::vector<int>& deviceIds) {
        std::map<int, DeviceRealtimeData> result;
        if (deviceIds.empty()) {
            co_return result;
        }

        std::shared_lock lock(mutex_);
        for (int deviceId : deviceIds) {
            auto it = cache_.find(deviceId);
            if (it != cache_.end() && !it->second.empty()) {
                result[deviceId] = it->second;
            }
        }
        co_return result;
    }

    Task<std::map<int, std::string>> getLatestReportTimes(const std::vector<int>& deviceIds) {
        std::map<int, std::string> result;
        if (deviceIds.empty()) {
            co_return result;
        }

        std::shared_lock lock(mutex_);
        for (int deviceId : deviceIds) {
            auto it = latestReportTimes_.find(deviceId);
            if (it != latestReportTimes_.end() && !it->second.empty()) {
                result[deviceId] = it->second;
            }
        }
        co_return result;
    }

    bool isInitialized() const {
        return initialized_.load(std::memory_order_acquire);
    }

    Task<void> initializeFromDb(const std::vector<int>& deviceIds) {
        while (true) {
            if (initialized_.load(std::memory_order_acquire)) {
                co_return;
            }

            bool expected = false;
            if (initializing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                break;
            }

            co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(50));
        }

        struct InitFlagGuard {
            std::atomic<bool>& flag;
            ~InitFlagGuard() {
                flag.store(false, std::memory_order_release);
            }
        } initGuard{initializing_};

        if (!deviceIds.empty()) {
            co_await loadFromDb(deviceIds);
        }

        initialized_.store(true, std::memory_order_release);
        LOG_DEBUG << "[RealtimeDataCache] Initialized from DB for " << deviceIds.size() << " devices";
    }

    Task<void> loadFromDb(const std::vector<int>& deviceIds) {
        if (deviceIds.empty()) {
            co_return;
        }

        auto [placeholders, params] = SqlHelper::buildParameterizedIn(deviceIds);
        params.push_back(std::to_string(Constants::DEVICE_DATA_LOOKBACK_DAYS));

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
        std::map<int, DeviceRealtimeData> loadedData;
        std::map<int, std::string> loadedLatestTimes;

        for (const auto& row : result) {
            int deviceId = FieldHelper::getInt(row["device_id"]);
            std::string funcCode = FieldHelper::getString(row["func_code"], "");
            if (funcCode.empty()) {
                continue;
            }

            std::string reportTime = FieldHelper::getString(row["report_time"], "");
            std::string dataStr = FieldHelper::getString(row["data"], "");

            Json::Value dataJson;
            std::string errs;
            std::istringstream iss(dataStr);
            if (!Json::parseFromStream(readerBuilder, iss, &dataJson, &errs)) {
                LOG_WARN << "[RealtimeDataCache] Failed to parse DB data for device " << deviceId << ": " << errs;
                continue;
            }

            loadedData[deviceId][funcCode] = {dataJson, reportTime};
            auto& latestTime = loadedLatestTimes[deviceId];
            if (latestTime.empty() || reportTime > latestTime) {
                latestTime = reportTime;
            }
        }

        batchUpdateMemory(loadedData, loadedLatestTimes);
    }

    void clearLatestTime(int deviceId) {
        std::unique_lock lock(mutex_);
        latestReportTimes_.erase(deviceId);
    }

    void invalidate(int deviceId) {
        std::unique_lock lock(mutex_);
        cache_.erase(deviceId);
        latestReportTimes_.erase(deviceId);
    }

    void invalidateAll() {
        std::unique_lock lock(mutex_);
        cache_.clear();
        latestReportTimes_.clear();
        initialized_.store(false, std::memory_order_release);
        initializing_.store(false, std::memory_order_release);
    }

private:
    RealtimeDataCache() = default;

    mutable std::shared_mutex mutex_;
    std::map<int, DeviceRealtimeData> cache_;
    std::map<int, std::string> latestReportTimes_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> initializing_{false};

    void updateMemory(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        if (funcCode.empty()) {
            return;
        }

        std::unique_lock lock(mutex_);
        cache_[deviceId][funcCode] = {data, reportTime};
        updateLatestTimeLocked(deviceId, reportTime);
    }

    void mergeUpdateMemory(int deviceId, const std::string& funcCode, const Json::Value& data, const std::string& reportTime) {
        if (funcCode.empty()) {
            return;
        }

        std::unique_lock lock(mutex_);
        auto& funcData = cache_[deviceId][funcCode];
        if (!funcData.data.isNull() &&
            funcData.data.isMember("data") &&
            data.isMember("data") &&
            funcData.data["data"].isObject() &&
            data["data"].isObject()) {
            Json::Value merged = funcData.data;
            const auto& newFields = data["data"];
            for (const auto& key : newFields.getMemberNames()) {
                merged["data"][key] = newFields[key];
            }
            funcData.data = std::move(merged);
            funcData.reportTime = reportTime;
        } else {
            funcData = {data, reportTime};
        }
        updateLatestTimeLocked(deviceId, reportTime);
    }

    void batchUpdateMemory(const std::map<int, DeviceRealtimeData>& dataMap,
                           const std::map<int, std::string>& latestTimeMap) {
        std::unique_lock lock(mutex_);
        for (const auto& [deviceId, deviceData] : dataMap) {
            auto& target = cache_[deviceId];
            for (const auto& [funcCode, funcData] : deviceData) {
                target[funcCode] = funcData;
            }
        }
        for (const auto& [deviceId, latestTime] : latestTimeMap) {
            updateLatestTimeLocked(deviceId, latestTime);
        }
    }

    void updateLatestTimeLocked(int deviceId, const std::string& reportTime) {
        if (reportTime.empty()) {
            return;
        }

        auto& latest = latestReportTimes_[deviceId];
        if (latest.empty() || reportTime > latest) {
            latest = reportTime;
        }
    }
};
