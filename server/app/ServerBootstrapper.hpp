#pragma once

#include "ApplicationModule.hpp"
#include "RuntimeModules.hpp"

#include "common/cache/DeviceCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/database/DatabaseInitializer.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/domain/EventBus.hpp"
#include "common/edgenode/AgentBridgeManager.hpp"

#include <drogon/drogon.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class StartupStageError final : public std::runtime_error {
public:
    StartupStageError(std::string stage, const std::exception& cause)
        : std::runtime_error(cause.what()), stage_(std::move(stage)) {}

    StartupStageError(std::string stage, std::string message)
        : std::runtime_error(message), stage_(std::move(stage)) {}

    const std::string& stage() const { return stage_; }

private:
    std::string stage_;
};

class ServerBootstrapper {
public:
    static ServerBootstrapper& instance() {
        static ServerBootstrapper bootstrapper;
        return bootstrapper;
    }

    void configureModules() {
        for (const auto& module : modules_) {
            module->configure();
        }
    }

    void registerModuleHandlers() {
        for (const auto& module : modules_) {
            module->registerHandlers();
        }
    }

    drogon::Task<> start() {
        co_await runStage("gb28181:start", [this]() -> drogon::Task<> {
            co_await module("gb28181").start();
        });

        co_await runStage("database:ping", []() -> drogon::Task<> {
            DatabaseService dbHealthCheck;
            co_await dbHealthCheck.ping();
        });

        co_await runStage("database:initialize", []() -> drogon::Task<> {
            co_await DatabaseInitializer::initialize();
        });

        co_await runStage("cache:invalidate", []() -> drogon::Task<> {
            DeviceCache::instance().markStale();
            co_return;
        });

        co_await runStage("cache:preload-device", []() -> drogon::Task<> {
            co_await DeviceCache::instance().getDevices();
        });

        co_await runStage("protocol:initialize", [this]() -> drogon::Task<> {
            co_await module("protocol").start();
        });

        co_await runStage("resource-version:reset", []() -> drogon::Task<> {
            ResourceVersion::instance().resetAll({
                "device", "user", "role", "menu", "department", "link",
                "protocol", "alert", "deviceGroup", "agent"
            });
            co_return;
        });

        co_await runStage("alert-engine:initialize", [this]() -> drogon::Task<> {
            co_await module("alert").start();
        });

        co_await runStage("agent:reset-online-status", []() -> drogon::Task<> {
            co_await AgentBridgeManager::instance().resetOnStartup();
        });

        co_await runStage("links:start-enabled", [this]() -> drogon::Task<> {
            co_await module("link").start();
        });
        co_return;
    }

    drogon::Task<> stop() {
        co_await module("gb28181").stop();
        co_await module("alert").stop();
        co_await module("link").stop();
        EventBus::instance().unsubscribeAll();
        DeviceCache::instance().invalidate();
        co_return;
    }

private:
    ServerBootstrapper() {
        modules_.push_back(std::make_unique<Gb28181RuntimeModule>());
        modules_.push_back(std::make_unique<ProtocolRuntimeModule>());
        modules_.push_back(std::make_unique<DomainEventRuntimeModule>());
        modules_.push_back(std::make_unique<AlertRuntimeModule>());
        modules_.push_back(std::make_unique<LinkRuntimeModule>());
    }

    ApplicationModule& module(std::string_view name) {
        for (const auto& mod : modules_) {
            if (mod->name() == name) {
                return *mod;
            }
        }
        throw StartupStageError(
            "startup:module-resolution",
            "Application module is not registered: " + std::string(name));
    }

    template<typename Fn>
    drogon::Task<> runStage(std::string stage, Fn&& fn) {
        LOG_INFO << "[Startup] " << stage;
        try {
            co_await fn();
        } catch (const StartupStageError&) {
            throw;
        } catch (const std::exception& e) {
            throw StartupStageError(std::move(stage), e);
        } catch (...) {
            throw StartupStageError(std::move(stage), "unknown exception");
        }
        co_return;
    }

    std::vector<std::unique_ptr<ApplicationModule>> modules_;
};
