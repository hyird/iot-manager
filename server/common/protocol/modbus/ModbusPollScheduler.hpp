#pragma once

#include "DtuRegistry.hpp"
#include "common/protocol/PollScheduler.hpp"

#include <functional>
#include <utility>
#include <vector>

namespace modbus {

/**
 * @brief Modbus 轮询调度器适配层
 *
 * 协议无关的 tick、失败降频、快读窗口、多 step 推进由 ProtocolPollScheduler 统一处理。
 * 本类只负责把 Modbus DTU/读组配置映射成通用 PollTask，并保留 DTU 绑定启停语义。
 */
class ModbusPollScheduler {
public:
    using EnqueueReadCallback = std::function<bool(int deviceId, size_t readGroupIndex)>;

    ModbusPollScheduler()
        : scheduler_("Modbus") {}

    void setEnqueueReadCallback(EnqueueReadCallback cb) {
        scheduler_.setEnqueueCallback(std::move(cb));
    }

    void reload(const DtuRegistry& registry) {
        std::vector<ProtocolPollScheduler::TaskConfig> tasks;
        auto definitions = registry.getAllDefinitions();

        for (const auto& dtu : definitions) {
            for (const auto& [slaveId, device] : dtu.devicesBySlave) {
                (void)slaveId;
                if (device.readGroups.empty()) {
                    continue;
                }

                ProtocolPollScheduler::TaskConfig task;
                task.deviceId = device.deviceId;
                task.deviceName = device.deviceName;
                task.groupKey = dtu.dtuKey;
                task.linkId = device.linkId;
                task.stepCount = device.readGroups.size();
                task.intervalSec = device.readInterval;
                task.fastReadDurationSec = device.commandFastReadDuration;
                task.fastReadIntervalSec = device.commandFastReadInterval;
                task.enabled = false;
                tasks.push_back(std::move(task));
            }
        }

        scheduler_.reload(tasks);
    }

    void onSessionBound(const DtuDefinition& dtu) {
        scheduler_.setGroupEnabled(dtu.dtuKey, true);
    }

    void onSessionUnbound(const std::string& dtuKey) {
        scheduler_.setGroupEnabled(dtuKey, false);
    }

    void triggerNow(int deviceId) {
        scheduler_.triggerNow(deviceId);
    }

    void activateFastRead(int deviceId, int durationSec = 60, int intervalSec = 1) {
        scheduler_.activateFastRead(deviceId, durationSec, intervalSec);
    }

    void onReadCompleted(int deviceId, size_t readGroupIndex, bool success) {
        scheduler_.onStepCompleted(deviceId, readGroupIndex, success);
    }

private:
    ProtocolPollScheduler scheduler_;
};

}  // namespace modbus
