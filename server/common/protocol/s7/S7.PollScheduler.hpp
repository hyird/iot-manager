#pragma once

#include "common/protocol/PollScheduler.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace s7 {

/**
 * @brief S7 轮询调度器适配层
 *
 * 统一轮询状态机在 ProtocolPollScheduler 中实现；S7 每个设备只有一个轮询 step。
 */
class S7PollScheduler {
public:
    struct DeviceConfig {
        int deviceId = 0;
        std::string deviceName;
        int readIntervalSec = 5;
        int fastReadDurationSec = 60;
        int fastReadIntervalSec = 1;
        bool enabled = true;
    };

    using EnqueuePollCallback = std::function<bool(int deviceId)>;

    S7PollScheduler()
        : scheduler_("S7") {}

    void setEnqueuePollCallback(EnqueuePollCallback cb) {
        enqueuePollCallback_ = std::move(cb);
        scheduler_.setEnqueueCallback([this](int deviceId, size_t stepIndex) {
            (void)stepIndex;
            return enqueuePollCallback_ && enqueuePollCallback_(deviceId);
        });
    }

    void reload(const std::vector<DeviceConfig>& devices) {
        std::vector<ProtocolPollScheduler::TaskConfig> tasks;
        tasks.reserve(devices.size());

        for (const auto& device : devices) {
            ProtocolPollScheduler::TaskConfig task;
            task.deviceId = device.deviceId;
            task.deviceName = device.deviceName;
            task.groupKey = std::to_string(device.deviceId);
            task.stepCount = 1;
            task.intervalSec = std::max(1, device.readIntervalSec);
            task.fastReadDurationSec = std::clamp(device.fastReadDurationSec, 0, 3600);
            task.fastReadIntervalSec = std::clamp(device.fastReadIntervalSec, 1, 60);
            task.enabled = device.enabled;
            tasks.push_back(std::move(task));
        }

        scheduler_.reload(tasks);
    }

    void triggerNow(int deviceId) {
        scheduler_.triggerNow(deviceId);
    }

    void resetInProgress(int deviceId) {
        scheduler_.resetInProgress(deviceId);
    }

    void activateFastRead(int deviceId, int durationSec = 60, int intervalSec = 1) {
        scheduler_.activateFastRead(deviceId, durationSec, intervalSec);
    }

    void onPollCompleted(int deviceId, bool success) {
        scheduler_.onStepCompleted(deviceId, 0, success);
    }

private:
    EnqueuePollCallback enqueuePollCallback_;
    ProtocolPollScheduler scheduler_;
};

}  // namespace s7
