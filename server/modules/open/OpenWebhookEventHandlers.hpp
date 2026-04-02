#pragma once

#include "common/domain/EventBus.hpp"
#include "modules/alert/domain/Events.hpp"
#include "modules/device/domain/Events.hpp"
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

        bus.subscribe<CommandDispatched>([](const CommandDispatched& event) -> Task<void> {
            OpenWebhookDispatcher::instance().dispatchCommandDispatched(
                event.aggregateId, event.funcCode, event.elements);
            co_return;
        });

        LOG_INFO << "[OpenWebhook] Event handlers registered";
    }
};
