#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 设备相关事件 ====================

struct DeviceCreated : DomainEvent {
    std::string deviceCode;

    DeviceCreated(int deviceId, std::string code)
        : DomainEvent("DeviceCreated", deviceId, "Device")
        , deviceCode(std::move(code)) {}
};

struct DeviceUpdated : DomainEvent {
    DeviceUpdated(int deviceId)
        : DomainEvent("DeviceUpdated", deviceId, "Device") {}
};

struct DeviceDeleted : DomainEvent {
    DeviceDeleted(int deviceId)
        : DomainEvent("DeviceDeleted", deviceId, "Device") {}
};
