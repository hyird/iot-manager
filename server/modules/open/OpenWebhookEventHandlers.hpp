#pragma once

#include "common/domain/EventBus.hpp"
#include "modules/alert/domain/Events.hpp"
#include "OpenWebhookDispatcher.hpp"

class OpenWebhookEventHandlers {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    static void registerAll() {
        auto& bus = EventBus::instance();

        bus.subscribe<AlertTriggered>([](const AlertTriggered& event) -> Task<void> {
            OpenWebhookDispatcher::instance().dispatchAlertTriggered(event);
            co_return;
        });

        bus.subscribe<AlertResolved>([](const AlertResolved& event) -> Task<void> {
            OpenWebhookDispatcher::instance().dispatchAlertResolved(event);
            co_return;
        });

        LOG_INFO << "[OpenWebhook] Alert event handlers registered";
    }
};
