#pragma once

#include "common/domain/DomainEvent.hpp"

struct DeviceGroupCreated : DomainEvent {
    DeviceGroupCreated(int groupId)
        : DomainEvent("DeviceGroupCreated", groupId, "DeviceGroup") {}
};

struct DeviceGroupUpdated : DomainEvent {
    DeviceGroupUpdated(int groupId)
        : DomainEvent("DeviceGroupUpdated", groupId, "DeviceGroup") {}
};

struct DeviceGroupDeleted : DomainEvent {
    DeviceGroupDeleted(int groupId)
        : DomainEvent("DeviceGroupDeleted", groupId, "DeviceGroup") {}
};
