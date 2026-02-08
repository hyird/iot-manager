#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 链路相关事件 ====================

struct LinkCreated : DomainEvent {
    std::string name;
    std::string mode;
    std::string ip;
    int port;

    LinkCreated(int linkId, std::string n, std::string m, std::string i, int p)
        : DomainEvent("LinkCreated", linkId, "Link")
        , name(std::move(n)), mode(std::move(m)), ip(std::move(i)), port(p) {}
};

struct LinkUpdated : DomainEvent {
    std::string name;
    std::string mode;
    std::string ip;
    int port;
    bool enabled;
    bool needReload;

    LinkUpdated(int linkId, std::string n, std::string m, std::string i, int p, bool e, bool reload)
        : DomainEvent("LinkUpdated", linkId, "Link")
        , name(std::move(n)), mode(std::move(m)), ip(std::move(i))
        , port(p), enabled(e), needReload(reload) {}
};

struct LinkDeleted : DomainEvent {
    LinkDeleted(int linkId)
        : DomainEvent("LinkDeleted", linkId, "Link") {}
};
