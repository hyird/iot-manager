#pragma once

#include "common/network/TcpLinkManager.hpp"

#include <algorithm>
#include <chrono>
#include <drogon/drogon.h>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace s7 {

class S7PollScheduler {
public:
    struct DeviceConfig {
        int deviceId = 0;
        std::string deviceName;
        int readIntervalSec = 5;
        bool enabled = true;
    };

    using EnqueuePollCallback = std::function<bool(int deviceId)>;

    ~S7PollScheduler() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopTickLocked();
    }

    void setEnqueuePollCallback(EnqueuePollCallback cb) {
        enqueuePollCallback_ = std::move(cb);
    }

    void reload(const std::vector<DeviceConfig>& devices);
    void triggerNow(int deviceId);
    void onPollCompleted(int deviceId, bool success);

private:
    static constexpr double TICK_INTERVAL_SEC = 1.0;
    static constexpr int RETRY_INTERVAL_SEC = 1;
    static constexpr int DEGRADE_THRESHOLD = 3;
    static constexpr int DEGRADE_INTERVAL_SEC = 10;

    struct PollEntry {
        int deviceId = 0;
        std::string deviceName;
        int readIntervalSec = 5;
        bool enabled = true;
        bool cycleInProgress = false;
        int consecutiveFailures = 0;
        std::chrono::steady_clock::time_point nextDueTime = std::chrono::steady_clock::now();
    };

    void ensureTickLocked();
    void stopTickLocked();
    void onTick();
    void resetEntryForRetry(PollEntry& entry, std::chrono::steady_clock::time_point now);
    void dispatchPolls(const std::vector<int>& deviceIds);

    std::map<int, PollEntry> pollEntries_;
    EnqueuePollCallback enqueuePollCallback_;
    trantor::EventLoop* loop_ = nullptr;
    trantor::TimerId tickTimerId_{0};
    bool tickActive_ = false;
    mutable std::mutex mutex_;
};

inline void S7PollScheduler::ensureTickLocked() {
    if (tickActive_ || pollEntries_.empty()) {
        return;
    }

    loop_ = TcpLinkManager::instance().getNextIoLoop();
    if (!loop_) {
        return;
    }

    tickTimerId_ = loop_->runEvery(TICK_INTERVAL_SEC, [this]() {
        try {
            onTick();
        } catch (const std::exception& e) {
            LOG_ERROR << "[S7][PollScheduler] Tick exception: " << e.what();
        } catch (...) {
            LOG_ERROR << "[S7][PollScheduler] Tick unknown exception";
        }
    });
    tickActive_ = true;
}

inline void S7PollScheduler::stopTickLocked() {
    if (!tickActive_ || !loop_) {
        return;
    }

    loop_->invalidateTimer(tickTimerId_);
    tickTimerId_ = trantor::TimerId{0};
    tickActive_ = false;
}

inline void S7PollScheduler::resetEntryForRetry(
    PollEntry& entry, std::chrono::steady_clock::time_point now) {
    entry.cycleInProgress = false;
    entry.nextDueTime = now + std::chrono::seconds(RETRY_INTERVAL_SEC);
}

inline void S7PollScheduler::dispatchPolls(const std::vector<int>& deviceIds) {
    if (!enqueuePollCallback_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    for (int deviceId : deviceIds) {
        if (enqueuePollCallback_(deviceId)) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pollEntries_.find(deviceId);
        if (it != pollEntries_.end()) {
            resetEntryForRetry(it->second, now);
        }
    }
}

inline void S7PollScheduler::reload(const std::vector<DeviceConfig>& devices) {
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::map<int, PollEntry> rebuilt;
        for (const auto& device : devices) {
                PollEntry entry;
                entry.deviceId = device.deviceId;
                entry.deviceName = device.deviceName;
                entry.readIntervalSec = (std::max)(1, device.readIntervalSec);
                entry.enabled = device.enabled;
                entry.nextDueTime = now;

            auto oldIt = pollEntries_.find(device.deviceId);
            if (oldIt != pollEntries_.end()) {
                entry.cycleInProgress = false;
                entry.consecutiveFailures = oldIt->second.consecutiveFailures;
            }

            rebuilt.emplace(entry.deviceId, std::move(entry));
        }

        pollEntries_ = std::move(rebuilt);
        if (pollEntries_.empty()) {
            stopTickLocked();
        } else {
            ensureTickLocked();
        }
    }
}

inline void S7PollScheduler::triggerNow(int deviceId) {
    std::vector<int> immediatePolls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pollEntries_.find(deviceId);
        if (it == pollEntries_.end() || !it->second.enabled) {
            return;
        }

        if (!it->second.cycleInProgress) {
            it->second.cycleInProgress = true;
            it->second.nextDueTime = std::chrono::steady_clock::now();
            immediatePolls.push_back(deviceId);
        } else {
            it->second.nextDueTime = std::chrono::steady_clock::now();
        }
    }

    dispatchPolls(immediatePolls);
}

inline void S7PollScheduler::onPollCompleted(int deviceId, bool success) {
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pollEntries_.find(deviceId);
    if (it == pollEntries_.end()) {
        return;
    }

    auto& entry = it->second;
    if (!entry.enabled) {
        entry.cycleInProgress = false;
        return;
    }

    if (!success) {
        entry.consecutiveFailures++;
        entry.cycleInProgress = false;

        int intervalSec = entry.readIntervalSec;
        if (entry.consecutiveFailures >= DEGRADE_THRESHOLD
            && entry.readIntervalSec < DEGRADE_INTERVAL_SEC) {
                intervalSec = DEGRADE_INTERVAL_SEC;
                if (entry.consecutiveFailures == DEGRADE_THRESHOLD) {
                    LOG_WARN << "[S7][PollScheduler] Device " << (entry.deviceName.empty() ? "S7-unknown" : entry.deviceName)
                             << "(id=" << deviceId << ")"
                             << " degraded after " << DEGRADE_THRESHOLD
                             << " consecutive failures, interval=" << intervalSec << "s";
                }
            }
            entry.nextDueTime = now + std::chrono::seconds(intervalSec);
            return;
        }

        if (entry.consecutiveFailures >= DEGRADE_THRESHOLD) {
            LOG_INFO << "[S7][PollScheduler] Device " << (entry.deviceName.empty() ? "S7-unknown" : entry.deviceName)
                     << "(id=" << deviceId << ")"
                     << " recovered from degraded state";
        }
    entry.consecutiveFailures = 0;
    entry.cycleInProgress = false;
    entry.nextDueTime = now + std::chrono::seconds(entry.readIntervalSec);
}

inline void S7PollScheduler::onTick() {
    std::vector<int> duePolls;
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [deviceId, entry] : pollEntries_) {
            if (!entry.enabled || entry.cycleInProgress) {
                continue;
            }
            if (now < entry.nextDueTime) {
                continue;
            }

            entry.cycleInProgress = true;
            duePolls.push_back(deviceId);
        }
    }

    dispatchPolls(duePolls);
}

}  // namespace s7
