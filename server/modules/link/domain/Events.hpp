#pragma once

#include "common/domain/DomainEvent.hpp"

// ==================== 链路相关事件 ====================

struct LinkCreated : DomainEvent {
    std::string name;
    std::string mode;
    std::string protocol;
    std::string ip;
    int port;
    Json::Value targets;
    int agentId;
    std::string agentInterface;
    std::string agentBindIp;
    int agentPrefixLength;
    std::string agentGateway;

    LinkCreated(int linkId, std::string n, std::string m, std::string proto, std::string i, int p,
                Json::Value clientTargets,
                int managedAgentId, std::string managedInterface, std::string managedBindIp,
                int managedPrefixLength, std::string managedGateway)
        : DomainEvent("LinkCreated", linkId, "Link")
        , name(std::move(n))
        , mode(std::move(m))
        , protocol(std::move(proto))
        , ip(std::move(i))
        , port(p)
        , targets(std::move(clientTargets))
        , agentId(managedAgentId)
        , agentInterface(std::move(managedInterface))
        , agentBindIp(std::move(managedBindIp))
        , agentPrefixLength(managedPrefixLength)
        , agentGateway(std::move(managedGateway)) {}
};

struct LinkUpdated : DomainEvent {
    std::string name;
    std::string mode;
    std::string protocol;
    std::string ip;
    int port;
    Json::Value targets;
    bool enabled;
    bool needReload;
    int agentId;
    std::string agentInterface;
    std::string agentBindIp;
    int agentPrefixLength;
    std::string agentGateway;

    LinkUpdated(int linkId, std::string n, std::string m, std::string proto, std::string i, int p,
                Json::Value clientTargets,
                bool e, bool reload, int managedAgentId, std::string managedInterface,
                std::string managedBindIp, int managedPrefixLength, std::string managedGateway)
        : DomainEvent("LinkUpdated", linkId, "Link")
        , name(std::move(n))
        , mode(std::move(m))
        , protocol(std::move(proto))
        , ip(std::move(i))
        , port(p)
        , targets(std::move(clientTargets))
        , enabled(e)
        , needReload(reload)
        , agentId(managedAgentId)
        , agentInterface(std::move(managedInterface))
        , agentBindIp(std::move(managedBindIp))
        , agentPrefixLength(managedPrefixLength)
        , agentGateway(std::move(managedGateway)) {}
};

struct LinkDeleted : DomainEvent {
    LinkDeleted(int linkId)
        : DomainEvent("LinkDeleted", linkId, "Link") {}
};
