#pragma once

#include <drogon/drogon.h>

#include <string_view>

/**
 * @brief Runtime module boundary for server-side capabilities.
 *
 * Modules own their internal wiring and expose a small, uniform lifecycle to
 * the application bootstrapper. This keeps main.cpp from depending on every
 * concrete runtime detail.
 */
class ApplicationModule {
public:
    virtual ~ApplicationModule() = default;

    virtual std::string_view name() const = 0;

    /**
     * @brief Synchronous configuration/registration hook.
     *
     * Runs before Drogon starts accepting traffic. Use it for cheap setup and
     * configuration validation that can fail fast.
     */
    virtual void configure() {}

    /**
     * @brief Register event handlers or callbacks after infrastructure exists.
     */
    virtual void registerHandlers() {}

    /**
     * @brief Start async module work after database/cache prerequisites are ready.
     */
    virtual drogon::Task<> start() { co_return; }

    /**
     * @brief Stop async resources during shutdown.
     */
    virtual drogon::Task<> stop() { co_return; }
};

