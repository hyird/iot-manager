#pragma once

#include "DtuRegistry.hpp"

#include "common/network/TcpLinkManager.hpp"

#include <algorithm>
#include <chrono>
#include <drogon/drogon.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace modbus {

/**
 * @brief DTU 轮询调度器
 *
 * 只负责决定“哪个设备何时应被投递为读任务”，
 * 实际发送由 ModbusSessionEngine 在 session 内串行执行。
 */
class ModbusPollScheduler {
public:
    using EnqueueReadCallback = std::function<bool(int deviceId, size_t readGroupIndex)>;

    ~ModbusPollScheduler() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopTickLocked();
    }

    void setEnqueueReadCallback(EnqueueReadCallback cb) {
        enqueueReadCallback_ = std::move(cb);
    }

    /** 重建调度计划 */
    void reload(const DtuRegistry& registry);

    /** 绑定成功后允许对应设备进入调度 */
    void onSessionBound(const DtuDefinition& dtu);

    /** 会话断开后暂停对应设备调度 */
    void onSessionUnbound(const std::string& dtuKey);

    /** 触发设备立即轮询 */
    void triggerNow(int deviceId);

    /** 某个读组完成后，推进同设备的下一组或下一轮 */
    void onReadCompleted(int deviceId, size_t readGroupIndex, bool success);

private:
    static constexpr double TICK_INTERVAL_SEC = 1.0;
    static constexpr int RETRY_INTERVAL_SEC = 1;
    static constexpr int DEGRADE_THRESHOLD = 3;        // 连续 N 次超时后降频
    static constexpr int DEGRADE_INTERVAL_SEC = 10;    // 降频后轮询间隔（秒），原始间隔已 >= 此值则不降频

    struct PollEntry {
        int deviceId = 0;
        std::string dtuKey;
        int linkId = 0;
        size_t nextGroupIndex = 0;
        size_t readGroupCount = 0;
        int readIntervalSec = 1;
        bool enabled = false;
        bool cycleInProgress = false;
        int consecutiveFailures = 0;
        std::chrono::steady_clock::time_point nextDueTime = std::chrono::steady_clock::now();
    };

    void ensureTickLocked();
    void stopTickLocked();
    void onTick();
    void resetEntryForRetry(PollEntry& entry, std::chrono::steady_clock::time_point now);
    void dispatchReads(const std::vector<std::pair<int, size_t>>& reads);

    std::map<int, PollEntry> pollEntries_;
    EnqueueReadCallback enqueueReadCallback_;
    trantor::EventLoop* loop_ = nullptr;
    trantor::TimerId tickTimerId_{0};
    bool tickActive_ = false;
    mutable std::mutex mutex_;
};

inline void ModbusPollScheduler::ensureTickLocked() {
    if (tickActive_ || pollEntries_.empty()) return;

    loop_ = TcpLinkManager::instance().getNextIoLoop();
    if (!loop_) return;

    tickTimerId_ = loop_->runEvery(TICK_INTERVAL_SEC, [this]() {
        try {
            onTick();
        } catch (const std::exception& e) {
            LOG_ERROR << "[Modbus][PollScheduler] Tick exception: " << e.what();
        } catch (...) {
            LOG_ERROR << "[Modbus][PollScheduler] Tick unknown exception";
        }
    });
    tickActive_ = true;
}

inline void ModbusPollScheduler::stopTickLocked() {
    if (!tickActive_ || !loop_) return;
    loop_->invalidateTimer(tickTimerId_);
    tickTimerId_ = trantor::TimerId{0};
    tickActive_ = false;
}

inline void ModbusPollScheduler::resetEntryForRetry(
    PollEntry& entry, std::chrono::steady_clock::time_point now) {
    entry.cycleInProgress = false;
    entry.nextGroupIndex = 0;
    entry.nextDueTime = now + std::chrono::seconds(RETRY_INTERVAL_SEC);
}

inline void ModbusPollScheduler::dispatchReads(const std::vector<std::pair<int, size_t>>& reads) {
    if (!enqueueReadCallback_) return;

    const auto now = std::chrono::steady_clock::now();
    for (const auto& [deviceId, readGroupIndex] : reads) {
        if (enqueueReadCallback_(deviceId, readGroupIndex)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pollEntries_.find(deviceId);
        if (it != pollEntries_.end()) {
            resetEntryForRetry(it->second, now);
        }
    }
}

inline void ModbusPollScheduler::reload(const DtuRegistry& registry) {
    std::vector<std::pair<int, size_t>> immediateReads;
    const auto now = std::chrono::steady_clock::now();
    auto definitions = registry.getAllDefinitions();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::map<int, PollEntry> rebuilt;
        std::map<std::string, int> dtuStagger;

        for (const auto& dtu : definitions) {
            for (const auto& [slaveId, device] : dtu.devicesBySlave) {
                (void)slaveId;
                if (device.readGroups.empty()) continue;

                PollEntry entry;
                entry.deviceId = device.deviceId;
                entry.dtuKey = dtu.dtuKey;
                entry.linkId = device.linkId;
                entry.readIntervalSec = (std::max)(1, device.readInterval);
                entry.readGroupCount = device.readGroups.size();
                entry.nextDueTime = now;

                auto oldIt = pollEntries_.find(device.deviceId);
                if (oldIt != pollEntries_.end()) {
                    entry.enabled = oldIt->second.enabled;
                    if (entry.enabled) {
                        int idx = dtuStagger[entry.dtuKey]++;
                        if (idx == 0) {
                            entry.cycleInProgress = true;
                            immediateReads.emplace_back(entry.deviceId, 0);
                        } else {
                            entry.cycleInProgress = false;
                            entry.nextDueTime = now + std::chrono::seconds(idx);
                        }
                    }
                }

                rebuilt.emplace(entry.deviceId, std::move(entry));
            }
        }

        pollEntries_ = std::move(rebuilt);
        if (pollEntries_.empty()) {
            stopTickLocked();
        } else {
            ensureTickLocked();
        }
    }

    dispatchReads(immediateReads);
}

inline void ModbusPollScheduler::onSessionBound(const DtuDefinition& dtu) {
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool hasMatched = false;
        for (auto& [deviceId, entry] : pollEntries_) {
            if (entry.dtuKey != dtu.dtuKey) continue;
            hasMatched = true;
            entry.enabled = true;
            entry.cycleInProgress = false;
            entry.nextGroupIndex = 0;
            entry.nextDueTime = now;
        }

        if (hasMatched) {
            ensureTickLocked();
        }
    }
}

inline void ModbusPollScheduler::onSessionUnbound(const std::string& dtuKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [deviceId, entry] : pollEntries_) {
        (void)deviceId;
        if (entry.dtuKey != dtuKey) continue;
        entry.enabled = false;
        entry.cycleInProgress = false;
        entry.nextGroupIndex = 0;
    }
}

inline void ModbusPollScheduler::triggerNow(int deviceId) {
    std::vector<std::pair<int, size_t>> immediateReads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pollEntries_.find(deviceId);
        if (it == pollEntries_.end() || !it->second.enabled || it->second.readGroupCount == 0) return;

        if (!it->second.cycleInProgress) {
            it->second.cycleInProgress = true;
            it->second.nextGroupIndex = 0;
            it->second.nextDueTime = std::chrono::steady_clock::now();
            immediateReads.emplace_back(deviceId, 0);
        } else {
            it->second.nextDueTime = std::chrono::steady_clock::now();
        }
    }

    dispatchReads(immediateReads);
}

inline void ModbusPollScheduler::onReadCompleted(int deviceId, size_t readGroupIndex, bool success) {
    std::vector<std::pair<int, size_t>> immediateReads;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pollEntries_.find(deviceId);
        if (it == pollEntries_.end()) return;

        auto& entry = it->second;
        if (!entry.enabled) {
            entry.cycleInProgress = false;
            entry.nextGroupIndex = 0;
            return;
        }

        if (!success) {
            entry.consecutiveFailures++;
            entry.cycleInProgress = false;
            entry.nextGroupIndex = 0;

            int intervalSec = entry.readIntervalSec;
            if (entry.consecutiveFailures >= DEGRADE_THRESHOLD
                && entry.readIntervalSec < DEGRADE_INTERVAL_SEC) {
                intervalSec = DEGRADE_INTERVAL_SEC;
                if (entry.consecutiveFailures == DEGRADE_THRESHOLD) {
                    LOG_WARN << "[Modbus][PollScheduler] Device " << deviceId
                             << " degraded after " << DEGRADE_THRESHOLD
                             << " consecutive failures, interval=" << intervalSec << "s";
                }
            }
            entry.nextDueTime = now + std::chrono::seconds(intervalSec);
        } else if (readGroupIndex + 1 < entry.readGroupCount) {
            if (entry.consecutiveFailures >= DEGRADE_THRESHOLD) {
                LOG_INFO << "[Modbus][PollScheduler] Device " << deviceId
                         << " recovered from degraded state";
            }
            entry.consecutiveFailures = 0;
            entry.cycleInProgress = true;
            entry.nextGroupIndex = readGroupIndex + 1;
            immediateReads.emplace_back(deviceId, entry.nextGroupIndex);
        } else {
            if (entry.consecutiveFailures >= DEGRADE_THRESHOLD) {
                LOG_INFO << "[Modbus][PollScheduler] Device " << deviceId
                         << " recovered from degraded state";
            }
            entry.consecutiveFailures = 0;
            entry.cycleInProgress = false;
            entry.nextGroupIndex = 0;
            entry.nextDueTime = now + std::chrono::seconds(entry.readIntervalSec);
        }
    }

    dispatchReads(immediateReads);
}

inline void ModbusPollScheduler::onTick() {
    std::vector<std::pair<int, size_t>> dueReads;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [deviceId, entry] : pollEntries_) {
            if (!entry.enabled || entry.readGroupCount == 0 || entry.cycleInProgress) continue;
            if (now < entry.nextDueTime) continue;

            entry.cycleInProgress = true;
            entry.nextGroupIndex = 0;
            dueReads.emplace_back(deviceId, 0);
        }
    }

    dispatchReads(dueReads);
}

}  // namespace modbus
