#pragma once

#include "common/domain/EventBus.hpp"
#include "common/network/WebSocketManager.hpp"
#include "modules/device/domain/Events.hpp"
#include "modules/link/domain/Events.hpp"
#include "modules/protocol/domain/Events.hpp"
#include "modules/system/domain/Events.hpp"
#include "modules/alert/domain/Events.hpp"

/**
 * @brief WebSocket 事件广播处理器
 *
 * 订阅所有领域事件，通过 WebSocket 推送给已连接的客户端。
 * 在应用启动时调用 registerAll() 注册。
 *
 * 推送事件类型：
 * - device:created / device:updated / device:deleted
 * - link:created / link:updated / link:deleted
 * - protocol:created / protocol:updated / protocol:deleted
 * - system:user / system:role / system:menu / system:department
 */
class WsEventHandlers {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    static void registerAll() {
        auto& bus = EventBus::instance();

        // ==================== Device 事件 ====================
        bus.subscribe<DeviceCreated>([](const DeviceCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            data["deviceCode"] = e.deviceCode;
            WebSocketManager::instance().broadcast("device:created", data);
            co_return;
        });

        bus.subscribe<DeviceUpdated>([](const DeviceUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            data["linkId"] = e.linkId;
            WebSocketManager::instance().broadcast("device:updated", data);
            co_return;
        });

        bus.subscribe<DeviceDeleted>([](const DeviceDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("device:deleted", data);
            co_return;
        });

        // ==================== Link 事件 ====================
        bus.subscribe<LinkCreated>([](const LinkCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            data["name"] = e.name;
            data["mode"] = e.mode;
            WebSocketManager::instance().broadcast("link:created", data);
            co_return;
        });

        bus.subscribe<LinkUpdated>([](const LinkUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            data["name"] = e.name;
            data["enabled"] = e.enabled;
            WebSocketManager::instance().broadcast("link:updated", data);
            co_return;
        });

        bus.subscribe<LinkDeleted>([](const LinkDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("link:deleted", data);
            co_return;
        });

        // ==================== ProtocolConfig 事件 ====================
        bus.subscribe<ProtocolConfigCreated>([](const ProtocolConfigCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            data["name"] = e.name;
            WebSocketManager::instance().broadcast("protocol:created", data);
            co_return;
        });

        bus.subscribe<ProtocolConfigUpdated>([](const ProtocolConfigUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("protocol:updated", data);
            co_return;
        });

        bus.subscribe<ProtocolConfigDeleted>([](const ProtocolConfigDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("protocol:deleted", data);
            co_return;
        });

        // ==================== System 事件（精简推送） ====================
        bus.subscribe<UserCreated>([](const UserCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:user", data);
            co_return;
        });
        bus.subscribe<UserUpdated>([](const UserUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:user", data);
            co_return;
        });
        bus.subscribe<UserDeleted>([](const UserDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:user", data);
            co_return;
        });

        bus.subscribe<RoleCreated>([](const RoleCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:role", data);
            co_return;
        });
        bus.subscribe<RoleUpdated>([](const RoleUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:role", data);
            co_return;
        });
        bus.subscribe<RoleDeleted>([](const RoleDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:role", data);
            co_return;
        });

        bus.subscribe<MenuCreated>([](const MenuCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:menu", data);
            co_return;
        });
        bus.subscribe<MenuUpdated>([](const MenuUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:menu", data);
            co_return;
        });
        bus.subscribe<MenuDeleted>([](const MenuDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:menu", data);
            co_return;
        });

        bus.subscribe<DepartmentCreated>([](const DepartmentCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:department", data);
            co_return;
        });
        bus.subscribe<DepartmentUpdated>([](const DepartmentUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:department", data);
            co_return;
        });
        bus.subscribe<DepartmentDeleted>([](const DepartmentDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("system:department", data);
            co_return;
        });

        // ==================== Alert 事件 ====================
        bus.subscribe<AlertTriggered>([](const AlertTriggered& e) -> Task<void> {
            Json::Value data;
            data["id"] = static_cast<Json::Int64>(e.recordId);
            data["deviceId"] = e.deviceId;
            data["ruleId"] = e.ruleId;
            data["severity"] = e.severity;
            data["message"] = e.message;
            data["deviceName"] = e.deviceName;
            WebSocketManager::instance().broadcast("alert:triggered", data);
            co_return;
        });

        bus.subscribe<AlertRuleCreated>([](const AlertRuleCreated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("alert:rule:created", data);
            co_return;
        });
        bus.subscribe<AlertRuleUpdated>([](const AlertRuleUpdated& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("alert:rule:updated", data);
            co_return;
        });
        bus.subscribe<AlertRuleDeleted>([](const AlertRuleDeleted& e) -> Task<void> {
            Json::Value data;
            data["id"] = e.aggregateId;
            WebSocketManager::instance().broadcast("alert:rule:deleted", data);
            co_return;
        });

        LOG_INFO << "[WS] Event handlers registered for all domain events";
    }
};
