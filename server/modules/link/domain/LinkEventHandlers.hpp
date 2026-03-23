#pragma once

#include "Events.hpp"
#include "modules/device/domain/Events.hpp"
#include "modules/protocol/domain/Events.hpp"
#include "common/domain/EventBus.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"

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
                LinkTransportFacade::instance().reload(
                    event.aggregateId,
                    event.name,
                    event.mode,
                    event.protocol,
                    event.ip,
                    static_cast<uint16_t>(event.port),
                    true,
                    event.agentId,
                    event.agentInterface,
                    event.agentBindIp,
                    event.agentPrefixLength,
                    event.agentGateway
                );
                LOG_INFO << "LinkEventHandler: Started link #" << event.aggregateId
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
                LinkTransportFacade::instance().reload(
                    event.aggregateId,
                    event.name,
                    event.mode,
                    event.protocol,
                    event.ip,
                    static_cast<uint16_t>(event.port),
                    event.enabled,
                    event.agentId,
                    event.agentInterface,
                    event.agentBindIp,
                    event.agentPrefixLength,
                    event.agentGateway
                );
                LOG_INFO << "LinkEventHandler: Reloaded link #" << event.aggregateId;
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to reload link #"
                          << event.aggregateId << ": " << e.what();
            }
            co_return;
        });

        // 链路删除 → 停止 TCP 连接
        bus.subscribe<LinkDeleted>([](const LinkDeleted& event) -> Task<void> {
            try {
                LinkTransportFacade::instance().stop(event.aggregateId);
                LOG_INFO << "LinkEventHandler: Stopped link #" << event.aggregateId;
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
                LinkTransportFacade::instance().disconnectServerClients(event.linkId);
                LOG_INFO << "LinkEventHandler: Disconnected all clients on link #" << event.linkId
                         << " due to registration/heartbeat change on device #" << event.aggregateId;
            } catch (const std::exception& e) {
                LOG_ERROR << "LinkEventHandler: Failed to disconnect clients on link #"
                          << event.linkId << ": " << e.what();
            }
            co_return;
        });

        // Agent 设备创建 → 触发 Agent 配置同步
        bus.subscribe<DeviceCreated>([](const DeviceCreated& event) -> Task<void> {
            if (event.agentId <= 0) co_return;
            AgentBridgeManager::instance().requestConfigSync(event.agentId);
            LOG_INFO << "LinkEventHandler: Triggered Agent #" << event.agentId
                     << " config sync after device #" << event.aggregateId << " created";
            co_return;
        });

        // Agent 设备更新 → 触发 Agent 配置同步
        bus.subscribe<DeviceUpdated>([](const DeviceUpdated& event) -> Task<void> {
            if (event.agentId <= 0) co_return;
            AgentBridgeManager::instance().requestConfigSync(event.agentId);
            LOG_INFO << "LinkEventHandler: Triggered Agent #" << event.agentId
                     << " config sync after device #" << event.aggregateId << " updated";
            co_return;
        });

        // Agent 设备删除 → 触发 Agent 配置同步
        bus.subscribe<DeviceDeleted>([](const DeviceDeleted& event) -> Task<void> {
            if (event.agentId <= 0) co_return;
            AgentBridgeManager::instance().requestConfigSync(event.agentId);
            LOG_INFO << "LinkEventHandler: Triggered Agent #" << event.agentId
                     << " config sync after device #" << event.aggregateId << " deleted";
            co_return;
        });

        // 协议配置更新 → 查找关联的 Agent 设备并触发配置同步
        bus.subscribe<ProtocolConfigUpdated>([](const ProtocolConfigUpdated& event) -> Task<void> {
            try {
                DatabaseService db;
                auto rows = co_await db.execSqlCoro(R"(
                    SELECT DISTINCT (d.protocol_params->>'agent_id')::INT AS agent_id
                    FROM device d
                    WHERE d.protocol_config_id = ?
                      AND d.link_id = 0
                      AND d.protocol_params->>'agent_id' IS NOT NULL
                      AND (d.protocol_params->>'agent_id')::INT > 0
                      AND d.deleted_at IS NULL
                )", {std::to_string(event.aggregateId)});

                for (const auto& row : rows) {
                    int agentId = FieldHelper::getInt(row["agent_id"]);
                    if (agentId > 0) {
                        AgentBridgeManager::instance().requestConfigSync(agentId);
                        LOG_INFO << "LinkEventHandler: Triggered Agent #" << agentId
                                 << " config sync after protocol_config #" << event.aggregateId << " updated";
                    }
                }
            } catch (const std::exception& e) {
                LOG_WARN << "LinkEventHandler: Failed to sync agents after protocol_config #"
                         << event.aggregateId << " updated: " << e.what();
            }
            co_return;
        });

        LOG_INFO << "LinkEventHandlers: All handlers registered";
    }
};
