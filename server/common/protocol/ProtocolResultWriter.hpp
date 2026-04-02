#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <set>
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
        std::vector<ParsedFrameResult> persistedBatch;
        std::vector<int64_t> persistedIds;
        bool fallbackToSingleSave = false;
        try {
            std::vector<CommandRepository::SaveItem> items;
            items.reserve(batch.size());
            for (const auto& r : batch) {
                items.push_back({r.deviceId, r.linkId, r.protocol, r.data, r.reportTime});
            }

            persistedIds = co_await CommandRepository::saveBatch(items);
            persistedBatch = batch;
            LOG_TRACE << "[ProtocolResultWriter] Batch saved: " << batch.size() << " records";
        } catch (const std::exception& e) {
            LOG_ERROR << "[ProtocolResultWriter] saveBatchResults failed: " << e.what()
                      << ", falling back to individual saves";
            totalBatchFallbacks_.fetch_add(1, std::memory_order_relaxed);
            fallbackToSingleSave = true;
        }

        if (fallbackToSingleSave) {
            size_t failedCount = 0;

            for (const auto& r : batch) {
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
            }

            if (failedCount > 0) {
                LOG_ERROR << "[ProtocolResultWriter] 批次回退: " << batch.size() << " 条中 "
                          << failedCount << " 条写入数据库失败（DB 可能不可用）";
            }
        }

        for (size_t i = 0; i < persistedBatch.size(); ++i) {
            const auto& r = persistedBatch[i];

            try {
                co_await RealtimeDataCache::instance().updateAsync(
                    r.deviceId, r.funcCode, r.data, r.reportTime
                );
            } catch (const std::exception& e) {
                LOG_WARN << "[ProtocolResultWriter] updateAsync failed for device="
                         << r.deviceId << ": " << e.what();
            }

            if (notifyCommandCompletion_ && r.commandCompletion && i < persistedIds.size()) {
                notifyCommandCompletion_(
                    r.commandCompletion->commandKey,
                    r.commandCompletion->responseCode,
                    r.commandCompletion->success,
                    persistedIds[i]
                );
            }

            try {
                co_await AlertEngine::instance().checkData(r.deviceId, r.data);
            } catch (const std::exception& e) {
                LOG_WARN << "[ProtocolResultWriter] checkData failed for device="
                         << r.deviceId << ": " << e.what();
            }
        }

        if (!persistedBatch.empty()) {
            ResourceVersion::instance().incrementVersion("device");
            co_await broadcastRealtimeViaWs(persistedBatch);
            OpenWebhookDispatcher::instance().dispatch(persistedBatch);
        }
    }

    Task<void> broadcastRealtimeViaWs(const std::vector<ParsedFrameResult>& batch) {
        if (WebSocketManager::instance().connectionCount() == 0) co_return;

        try {
            std::set<int> affectedIds;
            for (const auto& r : batch) {
                affectedIds.insert(r.deviceId);
            }

            auto cachedDevices = co_await DeviceCache::instance().getDevices();
            std::map<int, const DeviceCache::CachedDevice*> deviceMap;
            for (const auto& d : cachedDevices) {
                deviceMap[d.id] = &d;
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
                updates.append(DeviceDataTransformer::buildRealtimeItem(*it->second, data, latestTime, connectionChecker_));
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
};
