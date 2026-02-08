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
    int linkId = 0;
    bool registrationChanged = false;

    DeviceUpdated(int deviceId, int linkId, bool regChanged = false)
        : DomainEvent("DeviceUpdated", deviceId, "Device")
        , linkId(linkId)
        , registrationChanged(regChanged) {}
};

struct DeviceDeleted : DomainEvent {
    DeviceDeleted(int deviceId)
        : DomainEvent("DeviceDeleted", deviceId, "Device") {}
};
