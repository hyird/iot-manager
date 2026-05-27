#pragma once

#include "common/network/TcpLinkManager.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief 协议无关的设备轮询调度器
 *
 * 统一 Modbus / S7 等协议共有的轮询语义：
 * - 周期 tick 与到期投递
 * - 单设备轮询周期 in-progress 保护
 * - 多 step 轮询推进（Modbus 读组）；单 step 协议使用 stepCount=1
 * - 失败重试、连续失败降频
 * - 指令后快读窗口
 * - 按 groupKey 错峰和按组启停
 *
 * 实际报文构建、session 队列和 in-flight 处理仍由协议引擎负责。
 */
class ProtocolPollScheduler {
public:
    struct TaskConfig {
        int deviceId = 0;
        std::string deviceName;
        std::string groupKey;
        int linkId = 0;
        size_t stepCount = 1;
        int intervalSec = 1;
        int fastReadDurationSec = 60;
        int fastReadIntervalSec = 1;
        bool enabled = true;
    };

    using EnqueueCallback = std::function<bool(int deviceId, size_t stepIndex)>;

    explicit ProtocolPollScheduler(std::string logPrefix)
        : logPrefix_(std::move(logPrefix)) {}

    ~ProtocolPollScheduler() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopTickLocked();
    }

    void setEnqueueCallback(EnqueueCallback cb) {
        enqueueCallback_ = std::move(cb);
    }

    void reload(const std::vector<TaskConfig>& configs) {
        reload(configs, true);
    }

    void reload(const std::vector<TaskConfig>& configs, bool preserveInProgress) {
        std::vector<std::pair<int, size_t>> immediateSteps;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            std::map<int, PollEntry> rebuilt;
            std::map<std::string, int> groupStagger;

            for (const auto& config : configs) {
                if (config.deviceId <= 0 || config.stepCount == 0) {
                    continue;
                }

                PollEntry entry;
                entry.deviceId = config.deviceId;
                entry.deviceName = config.deviceName;
                entry.groupKey = config.groupKey;
                entry.linkId = config.linkId;
                entry.stepCount = config.stepCount;
                entry.intervalSec = std::max(1, config.intervalSec);
                entry.fastReadDurationSec = std::clamp(config.fastReadDurationSec, 0, 3600);
                entry.fastReadIntervalSec = std::clamp(config.fastReadIntervalSec, 1, 60);
                entry.enabled = config.enabled;
                entry.nextDueTime = now;

                auto oldIt = pollEntries_.find(config.deviceId);
                if (oldIt != pollEntries_.end()) {
                    entry.enabled = config.enabled;
                    entry.cycleInProgress = preserveInProgress && oldIt->second.cycleInProgress;
                    entry.consecutiveFailures = oldIt->second.consecutiveFailures;
                    entry.fastReadUntil = oldIt->second.fastReadUntil;
                    entry.nextDueTime = oldIt->second.nextDueTime;
                    entry.nextStepIndex = std::min(oldIt->second.nextStepIndex, entry.stepCount - 1);
                    if (!preserveInProgress && entry.enabled) {
                        entry.nextStepIndex = 0;
                        entry.nextDueTime = now;
                    }
                } else if (entry.enabled) {
                    const int idx = groupStagger[entry.groupKey]++;
                    if (idx == 0) {
                        entry.cycleInProgress = true;
                        entry.nextStepIndex = 0;
                        immediateSteps.emplace_back(entry.deviceId, 0);
                    } else {
                        entry.nextDueTime = now + std::chrono::seconds(idx);
                    }
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

        dispatchSteps(immediateSteps);
    }

    void setGroupEnabled(const std::string& groupKey, bool enabled) {
        if (groupKey.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool matched = false;
            for (auto& [deviceId, entry] : pollEntries_) {
                (void)deviceId;
                if (entry.groupKey != groupKey) {
                    continue;
                }
                matched = true;
                entry.enabled = enabled;
                entry.cycleInProgress = false;
                entry.nextStepIndex = 0;
                if (enabled) {
                    entry.nextDueTime = now;
                }
            }
            if (matched && enabled) {
                ensureTickLocked();
            }
        }
    }

    void triggerNow(int deviceId) {
        std::vector<std::pair<int, size_t>> immediateSteps;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pollEntries_.find(deviceId);
            if (it == pollEntries_.end() || !it->second.enabled || it->second.stepCount == 0) {
                return;
            }

            if (!it->second.cycleInProgress) {
                it->second.cycleInProgress = true;
                it->second.nextStepIndex = 0;
                it->second.nextDueTime = std::chrono::steady_clock::now();
                immediateSteps.emplace_back(deviceId, 0);
            } else {
                it->second.nextDueTime = std::chrono::steady_clock::now();
            }
        }

        dispatchSteps(immediateSteps);
    }

    void activateFastRead(int deviceId, int durationSec, int intervalSec) {
        if (durationSec <= 0) {
            return;
        }

        std::vector<std::pair<int, size_t>> immediateSteps;
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pollEntries_.find(deviceId);
            if (it == pollEntries_.end() || !it->second.enabled || it->second.stepCount == 0) {
                return;
            }

            auto& entry = it->second;
            entry.fastReadIntervalSec = std::clamp(intervalSec, 1, 60);
            entry.fastReadUntil = now + std::chrono::seconds(durationSec);
            entry.nextDueTime = now;

            if (!entry.cycleInProgress) {
                entry.cycleInProgress = true;
                entry.nextStepIndex = 0;
                immediateSteps.emplace_back(deviceId, 0);
            }
        }

        dispatchSteps(immediateSteps);
    }

    void onStepCompleted(int deviceId, size_t stepIndex, bool success) {
        std::vector<std::pair<int, size_t>> immediateSteps;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pollEntries_.find(deviceId);
            if (it == pollEntries_.end()) {
                return;
            }

            auto& entry = it->second;
            if (!entry.enabled) {
                entry.cycleInProgress = false;
                entry.nextStepIndex = 0;
                return;
            }

            if (!success) {
                entry.consecutiveFailures++;
                entry.cycleInProgress = false;
                entry.nextStepIndex = 0;

                int intervalSec = effectiveIntervalSec(entry, now);
                if (entry.consecutiveFailures >= DEGRADE_THRESHOLD
                    && entry.intervalSec < DEGRADE_INTERVAL_SEC) {
                    intervalSec = DEGRADE_INTERVAL_SEC;
                    if (entry.consecutiveFailures == DEGRADE_THRESHOLD) {
                        LOG_WARN << "[" << logPrefix_ << "][PollScheduler] Device "
                                 << deviceLabel(entry)
                                 << "(id=" << deviceId << ")"
                                 << " degraded after " << DEGRADE_THRESHOLD
                                 << " consecutive failures, interval=" << intervalSec << "s";
                    }
                }
                entry.nextDueTime = now + std::chrono::seconds(intervalSec);
                return;
            }

            if (entry.consecutiveFailures >= DEGRADE_THRESHOLD) {
                LOG_INFO << "[" << logPrefix_ << "][PollScheduler] Device "
                         << deviceLabel(entry)
                         << "(id=" << deviceId << ")"
                         << " recovered from degraded state";
            }
            entry.consecutiveFailures = 0;

            if (stepIndex + 1 < entry.stepCount) {
                entry.cycleInProgress = true;
                entry.nextStepIndex = stepIndex + 1;
                immediateSteps.emplace_back(deviceId, entry.nextStepIndex);
            } else {
                entry.cycleInProgress = false;
                entry.nextStepIndex = 0;
                entry.nextDueTime = now + std::chrono::seconds(effectiveIntervalSec(entry, now));
            }
        }

        dispatchSteps(immediateSteps);
    }

private:
    static constexpr double TICK_INTERVAL_SEC = 1.0;
    static constexpr int RETRY_INTERVAL_SEC = 1;
    static constexpr int DEGRADE_THRESHOLD = 3;
    static constexpr int DEGRADE_INTERVAL_SEC = 10;

    struct PollEntry {
        int deviceId = 0;
        std::string deviceName;
        std::string groupKey;
        int linkId = 0;
        size_t nextStepIndex = 0;
        size_t stepCount = 1;
        int intervalSec = 1;
        bool enabled = true;
        bool cycleInProgress = false;
        int consecutiveFailures = 0;
        std::chrono::steady_clock::time_point nextDueTime = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point fastReadUntil{};
        int fastReadDurationSec = 60;
        int fastReadIntervalSec = 1;
    };

    void ensureTickLocked() {
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
                LOG_ERROR << "[" << logPrefix_ << "][PollScheduler] Tick exception: " << e.what();
            } catch (...) {
                LOG_ERROR << "[" << logPrefix_ << "][PollScheduler] Tick unknown exception";
            }
        });
        tickActive_ = true;
    }

    void stopTickLocked() {
        if (!tickActive_ || !loop_) {
            return;
        }
        loop_->invalidateTimer(tickTimerId_);
        tickTimerId_ = trantor::TimerId{0};
        tickActive_ = false;
    }

    void resetEntryForRetry(PollEntry& entry, std::chrono::steady_clock::time_point now) {
        entry.cycleInProgress = false;
        entry.nextStepIndex = 0;
        entry.nextDueTime = now + std::chrono::seconds(RETRY_INTERVAL_SEC);
    }

    void dispatchSteps(const std::vector<std::pair<int, size_t>>& steps) {
        if (!enqueueCallback_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        for (const auto& [deviceId, stepIndex] : steps) {
            if (enqueueCallback_(deviceId, stepIndex)) {
                continue;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pollEntries_.find(deviceId);
            if (it != pollEntries_.end()) {
                resetEntryForRetry(it->second, now);
            }
        }
    }

    void onTick() {
        std::vector<std::pair<int, size_t>> dueSteps;
        const auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [deviceId, entry] : pollEntries_) {
                if (!entry.enabled || entry.stepCount == 0 || entry.cycleInProgress) {
                    continue;
                }
                if (now < entry.nextDueTime) {
                    continue;
                }

                entry.cycleInProgress = true;
                entry.nextStepIndex = 0;
                dueSteps.emplace_back(deviceId, 0);
            }
        }

        dispatchSteps(dueSteps);
    }

    static int effectiveIntervalSec(const PollEntry& entry, std::chrono::steady_clock::time_point now) {
        if (entry.fastReadUntil != std::chrono::steady_clock::time_point{} && now < entry.fastReadUntil) {
            return std::max(1, entry.fastReadIntervalSec);
        }
        return std::max(1, entry.intervalSec);
    }

    static std::string deviceLabel(const PollEntry& entry) {
        return entry.deviceName.empty() ? "unknown" : entry.deviceName;
    }

    std::string logPrefix_;
    std::map<int, PollEntry> pollEntries_;
    EnqueueCallback enqueueCallback_;
    trantor::EventLoop* loop_ = nullptr;
    trantor::TimerId tickTimerId_{0};
    bool tickActive_ = false;
    mutable std::mutex mutex_;
};
