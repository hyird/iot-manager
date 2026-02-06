#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 协议配置相关事件 ====================

struct ProtocolConfigCreated : DomainEvent {
    std::string protocol;
    std::string name;

    ProtocolConfigCreated(int configId, std::string p, std::string n)
        : DomainEvent("ProtocolConfigCreated", configId, "ProtocolConfig")
        , protocol(std::move(p)), name(std::move(n)) {}
};

struct ProtocolConfigUpdated : DomainEvent {
    ProtocolConfigUpdated(int configId)
        : DomainEvent("ProtocolConfigUpdated", configId, "ProtocolConfig") {}
};

struct ProtocolConfigDeleted : DomainEvent {
    ProtocolConfigDeleted(int configId)
        : DomainEvent("ProtocolConfigDeleted", configId, "ProtocolConfig") {}
};
