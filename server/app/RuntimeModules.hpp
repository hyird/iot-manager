#pragma once

#include "ApplicationModule.hpp"

#include "common/edgenode/AgentBridgeManager.hpp"
#include "common/network/TcpLinkManager.hpp"
#include "common/protocol/ProtocolDispatcher.hpp"
#include "common/protocol/modbus/Modbus.ProtocolAdapter.hpp"
#include "common/protocol/s7/S7.ProtocolAdapter.hpp"
#include "common/protocol/sl651/SL651.DeviceConfigProvider.hpp"
#include "common/protocol/sl651/SL651.ProtocolAdapter.hpp"
#include "common/utils/ConfigManager.hpp"
#include "common/utils/Constants.hpp"
#include "modules/alert/AlertEngine.hpp"
#include "modules/gb28181/Gb28181Module.hpp"
#include "modules/link/Link.Service.hpp"
#include "modules/link/domain/LinkEventHandlers.hpp"
#include "modules/open/OpenWebhookEventHandlers.hpp"
#include "modules/websocket/WsEventHandlers.hpp"

#include <memory>
#include <string_view>

class Gb28181RuntimeModule final : public ApplicationModule {
public:
    std::string_view name() const override { return "gb28181"; }

    void configure() override {
        Gb28181Module::instance().initialize();
    }

    drogon::Task<> start() override {
        co_await Gb28181Module::instance().startCoro();
    }

    drogon::Task<> stop() override {
        co_await Gb28181Module::instance().stopCoro();
    }
};

class ProtocolRuntimeModule final : public ApplicationModule {
public:
    std::string_view name() const override { return "protocol"; }

    void registerHandlers() override {
        TcpLinkManager::instance().initialize(ConfigManager::getNumberOfThreads());

        auto& dispatcher = ProtocolDispatcher::instance();
        dispatcher.initialize();

        static sl651::SL651DeviceConfigProvider sl651ConfigProvider;
        auto runtimeContext = dispatcher.buildRuntimeContext();
        dispatcher.registerAdapter(
            std::make_unique<sl651::SL651ProtocolAdapter>(runtimeContext, sl651ConfigProvider));
        dispatcher.registerAdapter(
            std::make_unique<modbus::ModbusProtocolAdapter>(runtimeContext));
        dispatcher.registerAdapter(
            std::make_unique<s7::S7ProtocolAdapter>(runtimeContext));

        dispatcher.startBackgroundTasks();

        AgentBridgeManager::instance().setIngressHandlers(
            [](int deviceId, const std::string& clientAddr, const std::string& data) {
                ProtocolDispatcher::instance().handleDeviceData(deviceId, clientAddr, data);
            },
            [](int agentId, const std::string& endpointId, const std::string& clientAddr, bool connected) {
                ProtocolDispatcher::instance().handleEndpointConnection(
                    agentId, endpointId, clientAddr, connected);
            });
        AgentBridgeManager::instance().setParsedDataHandler(
            [](std::vector<ParsedFrameResult>&& results) {
                ProtocolDispatcher::instance().submitParsedResults(std::move(results));
            });
        AgentBridgeManager::instance().setCommandResultCallback(
            [](const std::string& commandKey, const std::string& responseCode,
               bool success, int64_t responseRecordId) {
                ProtocolDispatcher::instance().notifyCommandCompletion(
                    commandKey, responseCode, success, responseRecordId);
            });

        AgentBridgeManager::instance().startHealthCheck(drogon::app().getLoop());
    }

    drogon::Task<> start() override {
        co_await ProtocolDispatcher::instance().initializeProtocolAsync(Constants::PROTOCOL_MODBUS);
        co_await ProtocolDispatcher::instance().initializeProtocolAsync(Constants::PROTOCOL_S7);
    }
};

class DomainEventRuntimeModule final : public ApplicationModule {
public:
    std::string_view name() const override { return "domain-events"; }

    void registerHandlers() override {
        LinkEventHandlers::registerAll();
        WsEventHandlers::registerAll();
        OpenWebhookEventHandlers::registerAll();
    }
};

class AlertRuntimeModule final : public ApplicationModule {
public:
    std::string_view name() const override { return "alert"; }

    drogon::Task<> start() override {
        co_await AlertEngine::instance().initialize();
        AlertEngine::instance().startOfflineChecker(drogon::app().getLoop());
    }

    drogon::Task<> stop() override {
        AlertEngine::instance().stopOfflineChecker();
        co_return;
    }
};

class LinkRuntimeModule final : public ApplicationModule {
public:
    std::string_view name() const override { return "link"; }

    drogon::Task<> start() override {
        LinkService linkService;
        co_await linkService.startAllEnabled();
    }

    drogon::Task<> stop() override {
        co_await TcpLinkManager::instance().stopAllCoro();
    }
};
