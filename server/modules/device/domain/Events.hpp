#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 设备相关事件 ====================

struct CommandDispatched : DomainEvent {
    std::string funcCode;
    Json::Value elements;

    CommandDispatched(int deviceId, std::string fc, Json::Value elems)
        : DomainEvent("CommandDispatched", deviceId, "Device")
        , funcCode(std::move(fc))
        , elements(std::move(elems)) {}
};

struct DeviceCreated : DomainEvent {
    std::string protocol;
    std::string deviceCode;
    int agentId = 0;  // > 0 表示 Agent 模式设备

    DeviceCreated(int deviceId, std::string p, std::string code, int agent = 0)
        : DomainEvent("DeviceCreated", deviceId, "Device")
        , protocol(std::move(p))
        , deviceCode(std::move(code))
        , agentId(agent) {}
};

struct DeviceUpdated : DomainEvent {
    int linkId = 0;
    std::string protocol;
    std::string deviceCode;
    bool registrationChanged = false;
    int agentId = 0;

    DeviceUpdated(int deviceId, int linkId, std::string p, std::string code,
                  bool regChanged = false, int agent = 0)
        : DomainEvent("DeviceUpdated", deviceId, "Device")
        , linkId(linkId)
        , protocol(std::move(p))
        , deviceCode(std::move(code))
        , registrationChanged(regChanged)
        , agentId(agent) {}
};

struct DeviceDeleted : DomainEvent {
    std::string protocol;
    std::string deviceCode;
    int agentId = 0;

    DeviceDeleted(int deviceId, std::string p, std::string code, int agent = 0)
        : DomainEvent("DeviceDeleted", deviceId, "Device")
        , protocol(std::move(p))
        , deviceCode(std::move(code))
        , agentId(agent) {}
};
