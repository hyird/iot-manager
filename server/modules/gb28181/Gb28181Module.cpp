#include "modules/gb28181/Gb28181Module.hpp"

#include "api/ApiServer.h"
#include "config/AppConfig.h"
#include "device/DeviceRegistry.h"
#include "media/StreamRegistry.h"
#include "media/ZlmClient.h"
#include "platform/WindowsFirewall.h"
#include "sip/SipServer.h"

#include <trantor/utils/Logger.h>
#include <drogon/drogon.h>

#include <stdexcept>

struct Gb28181Module::Impl {
    AppConfig config;
    bool initialized{false};
    bool running{false};
    std::string lastErrorMessage;
    std::unique_ptr<DeviceRegistry> deviceRegistry;
    std::unique_ptr<StreamRegistry> streamRegistry;
    std::unique_ptr<ZlmClient> zlmClient;
    std::unique_ptr<SipServer> sipServer;
    std::unique_ptr<ApiServer> apiServer;

    void initialize() {
        if (initialized) {
            return;
        }

        const auto& customConfig = drogon::app().getCustomConfig();
        if (!customConfig.isMember("gb28181")) {
            config.enabled = false;
            initialized = true;
            LOG_INFO << "[GB28181] Module disabled: custom_config.gb28181 is not configured";
            return;
        }

        config = AppConfig::fromJson(customConfig["gb28181"]);
        initialized = true;

        if (!config.enabled) {
            LOG_INFO << "[GB28181] Module disabled by configuration";
            return;
        }

        deviceRegistry = std::make_unique<DeviceRegistry>();
        streamRegistry = std::make_unique<StreamRegistry>();
        zlmClient = std::make_unique<ZlmClient>(config.media);
        sipServer = std::make_unique<SipServer>(config.sip, config.media, *deviceRegistry, *zlmClient);
        apiServer = std::make_unique<ApiServer>(config, *deviceRegistry, *streamRegistry, *sipServer);
        apiServer->registerRoutes(config.apiPrefix);

        LOG_INFO << "[GB28181] API routes registered under " << config.apiPrefix;
    }

    void start() {
        drogon::async_run([this]() -> drogon::Task<> {
            co_await startCoro();
        });
    }

    drogon::Task<> startCoro() {
        if (!initialized) {
            initialize();
        }
        if (!config.enabled || running) {
            co_return;
        }
        if (!sipServer) {
            throw std::runtime_error("GB28181 SIP server is not initialized");
        }

        if (config.autoFirewall) {
            ensureWindowsFirewallRules(config);
        }

        co_await sipServer->startCoro();
        running = true;
        LOG_INFO << "[GB28181] Module started, SIP id=" << config.sip.id
                                << ", domain=" << config.sip.domain
                                << ", listen=" << config.sip.host << ':' << config.sip.port;
    }

    void stop() {
        drogon::async_run([this]() -> drogon::Task<> {
            co_await stopCoro();
        });
    }

    drogon::Task<> stopCoro() {
        if (!running) {
            co_return;
        }
        if (sipServer) {
            co_await sipServer->stopCoro();
        }
        running = false;
        LOG_INFO << "[GB28181] Module stopped";
    }
};

Gb28181Module& Gb28181Module::instance() {
    static Gb28181Module module;
    return module;
}

Gb28181Module::Gb28181Module()
    : impl_(std::make_unique<Impl>()) {}

Gb28181Module::~Gb28181Module() = default;

void Gb28181Module::initialize() {
    try {
        impl_->initialize();
        impl_->lastErrorMessage.clear();
    } catch (const std::exception& e) {
        impl_->lastErrorMessage = e.what();
        throw;
    }
}

void Gb28181Module::start() {
    drogon::async_run([this]() -> drogon::Task<> {
        try {
            co_await startCoro();
        } catch (const std::exception& e) {
            impl_->lastErrorMessage = e.what();
            LOG_ERROR << "[GB28181] Async start failed: " << e.what();
        }
    });
}

drogon::Task<> Gb28181Module::startCoro() {
    try {
        co_await impl_->startCoro();
        impl_->lastErrorMessage.clear();
    } catch (const std::exception& e) {
        impl_->lastErrorMessage = e.what();
        throw;
    }
}

void Gb28181Module::stop() {
    drogon::async_run([this]() -> drogon::Task<> {
        try {
            co_await stopCoro();
        } catch (const std::exception& e) {
            impl_->lastErrorMessage = e.what();
            LOG_ERROR << "[GB28181] Async stop failed: " << e.what();
        }
    });
}

drogon::Task<> Gb28181Module::stopCoro() {
    co_await impl_->stopCoro();
}

bool Gb28181Module::enabled() const {
    return impl_->config.enabled;
}

bool Gb28181Module::started() const {
    return impl_->running;
}

const std::string& Gb28181Module::lastError() const {
    return impl_->lastErrorMessage;
}
