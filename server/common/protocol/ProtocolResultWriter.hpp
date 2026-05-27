#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "FrameResult.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/network/WebSocketManager.hpp"
#include "modules/alert/AlertEngine.hpp"
#include "modules/device/DeviceDataTransformer.hpp"
#include "modules/device/domain/CommandRepository.hpp"
#include "modules/open/OpenWebhookDispatcher.hpp"

/**
 * @brief 协议结果写入器
 *
 * 负责：
 * - 解析结果攒批
 * - 批量写入 device_data
 * - 更新实时缓存与资源版本
 * - 推送 WebSocket 实时数据
 * - 回调协议层的命令完成通知
 */
class ProtocolResultWriter {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    using CommandCompletionNotifier = std::function<void(
        const std::string& commandKey,
        const std::string& responseCode,
        bool success,
        int64_t responseRecordId)>;

    static constexpr size_t DEFAULT_BATCH_SIZE = 100;
    static constexpr double DEFAULT_FLUSH_INTERVAL_SEC = 0.2;

    using ConnectionChecker = std::function<bool(int deviceId)>;

    explicit ProtocolResultWriter(CommandCompletionNotifier notifier)
        : notifyCommandCompletion_(std::move(notifier)) {}

    void initialize(trantor::EventLoop* loop) {
        batchLoop_ = loop;
    }

    void setConnectionChecker(ConnectionChecker checker) {
        connectionChecker_ = std::move(checker);
    }

    void activateRealtimeStoreWindow(int deviceId, int durationSec = 60) {
        if (deviceId <= 0 || durationSec <= 0) return;

        std::lock_guard lock(storageMutex_);
        realtimeStoreUntil_[deviceId] =
            std::chrono::steady_clock::now() + std::chrono::seconds(durationSec);
    }

    trantor::EventLoop* eventLoop() const {
        return batchLoop_;
    }

    void submit(std::vector<ParsedFrameResult>&& results) {
        if (!batchLoop_ || results.empty()) return;

        batchLoop_->queueInLoop([this, results = std::move(results)]() mutable {
            enqueueBatchResults(std::move(results));
        });
    }

    int64_t batchFlushCount() const {
        return totalBatchFlushes_.load(std::memory_order_relaxed);
    }

    int64_t batchFallbackCount() const {
        return totalBatchFallbacks_.load(std::memory_order_relaxed);
    }

private:
    void enqueueBatchResults(std::vector<ParsedFrameResult>&& results) {
        for (auto& r : results) {
            pendingBatch_.push_back(std::move(r));
        }

        if (!batchTimerActive_ && !pendingBatch_.empty()) {
            batchTimerActive_ = true;
            batchTimerId_ = batchLoop_->runAfter(DEFAULT_FLUSH_INTERVAL_SEC, [this]() {
                try {
                    batchTimerActive_ = false;
                    flushBatch();
                } catch (const std::exception& e) {
                    LOG_ERROR << "[ProtocolResultWriter] flushBatch timer exception: " << e.what();
                } catch (...) {
                    LOG_ERROR << "[ProtocolResultWriter] flushBatch timer unknown exception";
                }
            });
        }

        if (pendingBatch_.size() >= DEFAULT_BATCH_SIZE) {
            if (batchTimerActive_) {
                batchLoop_->invalidateTimer(batchTimerId_);
                batchTimerActive_ = false;
            }
            flushBatch();
        }
    }

    void flushBatch() {
        if (pendingBatch_.empty()) return;

        totalBatchFlushes_.fetch_add(1, std::memory_order_relaxed);

        std::vector<ParsedFrameResult> batch;
        batch.swap(pendingBatch_);

        drogon::async_run([this, batch = std::move(batch)]() -> Task<> {
            try {
                co_await saveBatchResults(batch);
            } catch (const std::exception& e) {
                LOG_ERROR << "[ProtocolResultWriter] flushBatch failed: " << e.what();
            }
        });
    }

    Task<void> saveBatchResults(const std::vector<ParsedFrameResult>& batch) {
        std::vector<ParsedFrameResult> realtimeBatch = batch;
        auto persistBatch = filterPersistableResults(batch);
        std::vector<ParsedFrameResult> persistedBatch;
        std::vector<int64_t> persistedIds;
        bool fallbackToSingleSave = false;

        if (!persistBatch.empty()) {
            try {
                std::vector<CommandRepository::SaveItem> items;
                items.reserve(persistBatch.size());
                for (const auto& r : persistBatch) {
                    items.push_back({r.deviceId, r.linkId, r.protocol, r.data, r.reportTime});
                }

                persistedIds = co_await CommandRepository::saveBatch(items);
                persistedBatch = persistBatch;
                markPersistedResults(persistedBatch);
                LOG_TRACE << "[ProtocolResultWriter] Batch saved: " << persistBatch.size() << " records";
            } catch (const std::exception& e) {
                LOG_ERROR << "[ProtocolResultWriter] saveBatchResults failed: " << e.what()
                          << ", falling back to individual saves";
                totalBatchFallbacks_.fetch_add(1, std::memory_order_relaxed);
                fallbackToSingleSave = true;
            }

            if (fallbackToSingleSave) {
                size_t failedCount = 0;

                for (const auto& r : persistBatch) {
                    int64_t savedId = 0;

                    try {
                        savedId = co_await CommandRepository::save(
                            r.deviceId, r.linkId, r.protocol, r.data, r.reportTime);
                    } catch (const std::exception& e) {
                        ++failedCount;
                        if (failedCount == 1) {
                            LOG_ERROR << "[ProtocolResultWriter] 逐条写入也失败: " << e.what();
                        }
                        continue;
                    }

                    persistedBatch.push_back(r);
                    persistedIds.push_back(savedId);
                    markPersistedResults({r});
                }

                if (failedCount > 0) {
                    LOG_ERROR << "[ProtocolResultWriter] 批次回退: " << persistBatch.size() << " 条中 "
                              << failedCount << " 条写入数据库失败（DB 可能不可用）";
                }
            }
        }

        for (const auto& r : realtimeBatch) {

            try {
                co_await RealtimeDataCache::instance().mergeUpdateAsync(
                    r.deviceId, r.funcCode, r.data, r.reportTime
                );
            } catch (const std::exception& e) {
                LOG_WARN << "[ProtocolResultWriter] mergeUpdateAsync failed for device="
                         << r.deviceId << ": " << e.what();
            }

            try {
                co_await AlertEngine::instance().checkData(r.deviceId, r.data);
            } catch (const std::exception& e) {
                LOG_WARN << "[ProtocolResultWriter] checkData failed for device="
                         << r.deviceId << ": " << e.what();
            }
        }

        for (size_t i = 0; i < persistedBatch.size(); ++i) {
            const auto& r = persistedBatch[i];

            if (notifyCommandCompletion_ && r.commandCompletion && i < persistedIds.size()) {
                notifyCommandCompletion_(
                    r.commandCompletion->commandKey,
                    r.commandCompletion->responseCode,
                    r.commandCompletion->success,
                    persistedIds[i]
                );
            }
        }

        if (!realtimeBatch.empty()) {
            ResourceVersion::instance().incrementVersion("device");
            co_await broadcastRealtimeViaWs(realtimeBatch);
            OpenWebhookDispatcher::instance().dispatchMergedDataReports(realtimeBatch);
            OpenWebhookDispatcher::instance().dispatch(realtimeBatch);
        }
    }

    std::vector<ParsedFrameResult> filterPersistableResults(const std::vector<ParsedFrameResult>& batch) {
        std::vector<ParsedFrameResult> result;
        result.reserve(batch.size());

        const auto steadyNow = std::chrono::steady_clock::now();
        std::map<int, std::chrono::system_clock::time_point> stagedLastTimes;
        std::map<int, Json::Value> stagedLastData;

        std::lock_guard lock(storageMutex_);
        pruneRealtimeStoreWindowsLocked(steadyNow);

        for (const auto& r : batch) {
            if (shouldPersistResultLocked(r, steadyNow, stagedLastTimes, stagedLastData)) {
                result.push_back(r);
            }
        }

        return result;
    }

    bool shouldPersistResultLocked(
        const ParsedFrameResult& result,
        std::chrono::steady_clock::time_point steadyNow,
        std::map<int, std::chrono::system_clock::time_point>& stagedLastTimes,
        std::map<int, Json::Value>& stagedLastData) const {
        if (result.deviceId <= 0) {
            return true;
        }
        if (result.commandCompletion.has_value()) {
            return true;
        }

        const auto direction = result.data.get("direction", "UP").asString();
        if (direction != "UP") {
            return true;
        }

        auto fastIt = realtimeStoreUntil_.find(result.deviceId);
        if (fastIt != realtimeStoreUntil_.end() && steadyNow < fastIt->second) {
            auto stagedDataIt = stagedLastData.find(result.deviceId);
            auto lastDataIt = lastStoredData_.find(result.deviceId);
            const Json::Value* lastData = stagedDataIt != stagedLastData.end()
                ? &stagedDataIt->second
                : (lastDataIt != lastStoredData_.end() ? &lastDataIt->second : nullptr);
            if (lastData != nullptr && *lastData == result.data) {
                return false;
            }
            stagedLastData[result.deviceId] = result.data;
            return true;
        }

        auto device = DeviceCache::instance().findByIdSync(result.deviceId);
        const int storageIntervalSec = device ? std::max(1, device->storageInterval) : 1;
        if (storageIntervalSec <= 1) {
            return true;
        }

        const auto reportTime = parseReportTime(result.reportTime)
            .value_or(std::chrono::system_clock::now());
        auto stagedIt = stagedLastTimes.find(result.deviceId);
        const auto lastIt = lastStoredReportTimes_.find(result.deviceId);
        const auto lastTime = stagedIt != stagedLastTimes.end()
            ? stagedIt->second
            : (lastIt != lastStoredReportTimes_.end()
                ? lastIt->second
                : std::chrono::system_clock::time_point{});

        if (lastTime != std::chrono::system_clock::time_point{}
            && reportTime < lastTime + std::chrono::seconds(storageIntervalSec)) {
            return false;
        }

        stagedLastTimes[result.deviceId] = reportTime;
        return true;
    }

    void markPersistedResults(const std::vector<ParsedFrameResult>& results) {
        if (results.empty()) return;

        std::lock_guard lock(storageMutex_);
        for (const auto& r : results) {
            if (r.deviceId <= 0) continue;
            auto reportTime = parseReportTime(r.reportTime)
                .value_or(std::chrono::system_clock::now());
            auto& last = lastStoredReportTimes_[r.deviceId];
            if (last == std::chrono::system_clock::time_point{} || reportTime > last) {
                last = reportTime;
            }
            lastStoredData_[r.deviceId] = r.data;
        }
    }

    void pruneRealtimeStoreWindowsLocked(std::chrono::steady_clock::time_point now) {
        for (auto it = realtimeStoreUntil_.begin(); it != realtimeStoreUntil_.end();) {
            if (now >= it->second) {
                it = realtimeStoreUntil_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static std::optional<std::chrono::system_clock::time_point> parseReportTime(const std::string& reportTime) {
        if (reportTime.empty()) return std::nullopt;

        std::string timePart = reportTime;
        if (!timePart.empty() && timePart.back() == 'Z') {
            timePart.pop_back();
        }

        int offsetSeconds = 0;
        bool hasOffset = false;
        size_t pos = timePart.find_last_of("+-");
        if (pos != std::string::npos && pos > 10) {
            std::string offsetStr = timePart.substr(pos);
            if (offsetStr.size() == 6 && offsetStr[3] == ':') {
                try {
                    const int hours = std::stoi(offsetStr.substr(1, 2));
                    const int mins = std::stoi(offsetStr.substr(4, 2));
                    offsetSeconds = hours * 3600 + mins * 60;
                    if (offsetStr[0] == '-') {
                        offsetSeconds = -offsetSeconds;
                    }
                    hasOffset = true;
                    timePart = timePart.substr(0, pos);
                } catch (...) {
                    return std::nullopt;
                }
            }
        }

        std::tm tm{};
        std::istringstream iss(timePart);
        iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
        if (iss.fail()) return std::nullopt;

#ifdef _WIN32
        auto tt = _mkgmtime(&tm);
#else
        auto tt = timegm(&tm);
#endif
        if (tt == -1) return std::nullopt;

        auto tp = std::chrono::system_clock::from_time_t(tt);
        if (hasOffset) {
            tp -= std::chrono::seconds(offsetSeconds);
        }
        return tp;
    }

    Task<void> broadcastRealtimeViaWs(const std::vector<ParsedFrameResult>& batch) {
        if (WebSocketManager::instance().connectionCount() == 0) co_return;

        try {
            std::set<int> affectedIds;
            for (const auto& r : batch) {
                affectedIds.insert(r.deviceId);
            }

            std::map<int, DeviceCache::CachedDevice> deviceMap;
            std::vector<int> missingIds;
            for (int deviceId : affectedIds) {
                if (auto device = DeviceCache::instance().findByIdSync(deviceId)) {
                    deviceMap.emplace(deviceId, std::move(*device));
                } else {
                    missingIds.push_back(deviceId);
                }
            }

            if (!missingIds.empty()) {
                auto cachedDevices = co_await DeviceCache::instance().getDevices();
                for (const auto& d : cachedDevices) {
                    if (affectedIds.count(d.id) > 0) {
                        deviceMap.emplace(d.id, d);
                    }
                }
            }

            auto& realtimeCache = RealtimeDataCache::instance();
            Json::Value updates(Json::arrayValue);

            for (int deviceId : affectedIds) {
                auto it = deviceMap.find(deviceId);
                if (it == deviceMap.end()) continue;

                auto realtimeData = co_await realtimeCache.get(deviceId);
                auto latestTime = co_await realtimeCache.getLatestReportTime(deviceId);

                RealtimeDataCache::DeviceRealtimeData emptyData;
                const auto& data = realtimeData ? *realtimeData : emptyData;
                updates.append(DeviceDataTransformer::buildRealtimeItem(it->second, data, latestTime, connectionChecker_));
            }

            Json::Value payload(Json::objectValue);
            payload["updates"] = std::move(updates);
            WebSocketManager::instance().broadcast("device:realtime", payload);
        } catch (const std::exception& e) {
            LOG_WARN << "[ProtocolResultWriter] broadcastRealtimeViaWs failed: " << e.what();
        }
    }

    CommandCompletionNotifier notifyCommandCompletion_;
    ConnectionChecker connectionChecker_;
    trantor::EventLoop* batchLoop_ = nullptr;
    std::vector<ParsedFrameResult> pendingBatch_;
    trantor::TimerId batchTimerId_{0};
    bool batchTimerActive_ = false;
    std::atomic<int64_t> totalBatchFlushes_{0};
    std::atomic<int64_t> totalBatchFallbacks_{0};
    mutable std::mutex storageMutex_;
    std::map<int, std::chrono::system_clock::time_point> lastStoredReportTimes_;
    std::map<int, Json::Value> lastStoredData_;
    std::map<int, std::chrono::steady_clock::time_point> realtimeStoreUntil_;
};
