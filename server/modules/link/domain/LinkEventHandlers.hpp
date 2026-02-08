#pragma once

#include "Events.hpp"
#include "modules/device/domain/Events.hpp"
#include "common/domain/EventBus.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/utils/Constants.hpp"

/**
 * @brief Link 事件处理器
 *
 * 将 TCP 连接管理从 Link 聚合根中解耦出来。
 * 聚合根只负责业务逻辑和持久化，TCP 连接的启停由事件驱动。
 */
class LinkEventHandlers {
public:
    template<typename T = void> using Task = drogon::Task<T>;
    /**
     * @brief 注册所有 Link 事件处理器（应用启动时调用）
     */
    static void registerAll() {
        auto& bus = EventBus::instance();

        // 链路创建 → 启动 TCP 连接
        bus.subscribe<LinkCreated>([](const LinkCreated& event) -> Task<void> {
            try {
                startTcpLink(event.aggregateId, event.name, event.mode,
                             event.ip, static_cast<uint16_t>(event.port));
                LOG_INFO << "LinkEventHandler: Started TCP link #" << event.aggregateId
                         << " (" << event.mode << " " << event.ip << ":" << event.port << ")";
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to start link #"
                          << event.aggregateId << ": " << e.what();
            }
            co_return;
        });

        // 链路更新 → 重载 TCP 连接
        bus.subscribe<LinkUpdated>([](const LinkUpdated& event) -> Task<void> {
            if (!event.needReload) co_return;

            try {
                TcpLinkManager::instance().reload(
                    event.aggregateId, event.name, event.mode,
                    event.ip, static_cast<uint16_t>(event.port),
                    event.enabled
                );
                LOG_INFO << "LinkEventHandler: Reloaded TCP link #" << event.aggregateId;
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to reload link #"
                          << event.aggregateId << ": " << e.what();
            }
            co_return;
        });

        // 链路删除 → 停止 TCP 连接
        bus.subscribe<LinkDeleted>([](const LinkDeleted& event) -> Task<void> {
            try {
                TcpLinkManager::instance().stop(event.aggregateId);
                LOG_INFO << "LinkEventHandler: Stopped TCP link #" << event.aggregateId;
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to stop link #"
                          << event.aggregateId << ": " << e.what();
            }
            co_return;
        });

        // 设备注册包/心跳包变更 → 断开链路所有连接，强制设备重新注册
        bus.subscribe<DeviceUpdated>([](const DeviceUpdated& event) -> Task<void> {
            if (!event.registrationChanged || event.linkId <= 0) co_return;

            try {
                TcpLinkManager::instance().disconnectServerClients(event.linkId);
                LOG_INFO << "LinkEventHandler: Disconnected all clients on link #" << event.linkId
                         << " due to registration/heartbeat change on device #" << event.aggregateId;
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to disconnect clients on link #"
                          << event.linkId << ": " << e.what();
            }
            co_return;
        });

        LOG_INFO << "LinkEventHandlers: All handlers registered";
    }

private:
    static void startTcpLink(int linkId, const std::string& name,
                             const std::string& mode, const std::string& ip, uint16_t port) {
        if (mode == Constants::LINK_MODE_TCP_SERVER) {
            TcpLinkManager::instance().startServer(linkId, name, ip, port);
        } else if (mode == Constants::LINK_MODE_TCP_CLIENT) {
            TcpLinkManager::instance().startClient(linkId, name, ip, port);
        } else {
            LOG_WARN << "LinkEventHandler: Unsupported link mode: " << mode;
        }
    }
};
