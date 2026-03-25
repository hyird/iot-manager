#pragma once

#include "common/network/TcpLinkManager.hpp"
#include "common/utils/Constants.hpp"

#include <set>

/**
 * @brief 链路传输门面
 *
 * 管理本地 TCP 链路的传输。Agent 设备使用 link_id=0，
 * 不经过 LinkTransportFacade，而是通过 AgentBridgeManager 直接路由。
 *
 * 链路表中仍可能存在 agent_id > 0 的历史记录（迁移后遗留），
 * 这些链路不启动本地 TCP，传输由 Agent 通过 WebSocket 处理。
 */
class LinkTransportFacade {
public:
    struct Route {
        bool managedByAgent = false;
        std::string mode;
        std::string ip;
        uint16_t port = 0;
    };

    static LinkTransportFacade& instance() {
        static LinkTransportFacade inst;
        return inst;
    }

    void reload(int linkId,
                const std::string& name,
                const std::string& mode,
                const std::string& protocol,
                const std::string& ip,
                uint16_t port,
                bool enabled,
                int agentId = 0,
                const std::string& agentInterface = "",
                const std::string& agentBindIp = "",
                int agentPrefixLength = 0,
                const std::string& agentGateway = "") {
        // 忽略未使用的 Agent 参数（保持调用兼容性）
        (void)protocol;
        (void)agentInterface;
        (void)agentBindIp;
        (void)agentPrefixLength;
        (void)agentGateway;

        if (!enabled) {
            stop(linkId);
            return;
        }

        Route route;
        route.managedByAgent = agentId > 0;
        route.mode = mode;
        route.ip = ip;
        route.port = port;

        {
            std::unique_lock lock(mutex_);
            routes_[linkId] = route;
        }

        // Agent 管理的链路不启动本地 TCP（Agent 通过 WebSocket 处理传输）
        if (route.managedByAgent) {
            return;
        }

        if (mode == Constants::LINK_MODE_TCP_SERVER) {
            TcpLinkManager::instance().startServer(linkId, name, ip, port);
        } else if (mode == Constants::LINK_MODE_TCP_CLIENT) {
            TcpLinkManager::instance().startClient(linkId, name, ip, port);
        }
    }

    void stop(int linkId) {
        bool wasAgentManaged = false;
        {
            std::unique_lock lock(mutex_);
            auto it = routes_.find(linkId);
            if (it != routes_.end()) {
                wasAgentManaged = it->second.managedByAgent;
                routes_.erase(it);
            }
        }

        // Agent 管理的链路没有本地 TCP 实例，无需停止
        if (!wasAgentManaged) {
            TcpLinkManager::instance().stop(linkId);
        }
    }

    Json::Value getStatus(int linkId) const {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(linkId);
        if (it != routes_.end() && it->second.managedByAgent) {
            // Agent 管理的链路返回占位状态
            Json::Value status(Json::objectValue);
            status["link_id"] = linkId;
            status["conn_status"] = "agent_managed";
            status["client_count"] = 0;
            status["clients"] = Json::Value(Json::arrayValue);
            return status;
        }
        return TcpLinkManager::instance().getStatus(linkId);
    }

    bool sendData(int linkId, const std::string& data) const {
        if (isAgentManaged(linkId)) {
            return false;  // Agent 设备使用 deviceId 路由，不经过 linkId
        }
        return TcpLinkManager::instance().sendData(linkId, data);
    }

    bool sendDataExcluding(int linkId, const std::string& data, const std::set<std::string>& excludeAddrs) const {
        if (isAgentManaged(linkId)) {
            return false;
        }
        return TcpLinkManager::instance().sendDataExcluding(linkId, data, excludeAddrs);
    }

    bool sendToClient(int linkId, const std::string& clientAddr, const std::string& data) const {
        if (isAgentManaged(linkId)) {
            return false;
        }
        return TcpLinkManager::instance().sendToClient(linkId, clientAddr, data);
    }

    void disconnectServerClient(int linkId, const std::string& clientAddr) const {
        if (isAgentManaged(linkId)) {
            return;
        }
        TcpLinkManager::instance().disconnectServerClient(linkId, clientAddr);
    }

    void disconnectServerClients(int linkId) const {
        if (isAgentManaged(linkId)) {
            return;
        }
        TcpLinkManager::instance().disconnectServerClients(linkId);
    }

private:
    LinkTransportFacade() = default;

    bool isAgentManaged(int linkId) const {
        std::shared_lock lock(mutex_);
        auto it = routes_.find(linkId);
        return it != routes_.end() && it->second.managedByAgent;
    }

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<int, Route> routes_;
};
