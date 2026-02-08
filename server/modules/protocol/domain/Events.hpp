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
    std::string protocol;
    std::string name;

    ProtocolConfigUpdated(int configId, std::string p, std::string n)
        : DomainEvent("ProtocolConfigUpdated", configId, "ProtocolConfig")
        , protocol(std::move(p))
        , name(std::move(n)) {}
};

struct ProtocolConfigDeleted : DomainEvent {
    std::string protocol;
    std::string name;

    ProtocolConfigDeleted(int configId, std::string p, std::string n)
        : DomainEvent("ProtocolConfigDeleted", configId, "ProtocolConfig")
        , protocol(std::move(p))
        , name(std::move(n)) {}
};
