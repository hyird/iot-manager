#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <utility>

#include "DomainEvent.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/cache/RealtimeDataCache.hpp"
#include "common/cache/ResourceVersion.hpp"

/**
 * @brief 事件处理器类型
 */
using EventHandler = std::function<drogon::Task<void>(const DomainEvent&)>;
using BuiltinEffectHandler = std::function<drogon::Task<void>(const DomainEvent&)>;

/**
 * @brief 事件总线 - 发布订阅模式
 *
 * 负责：
 * - 接收领域事件
 * - 分发给对应的处理器
 * - 处理缓存失效、资源版本更新等副作用
 *
 * 使用示例：
 * @code
 * // 发布事件
 * co_await EventBus::instance().publish(UserCreated{userId, username});
 *
 * // 订阅事件（在初始化时）
 * EventBus::instance().subscribe<UserCreated>([](const UserCreated& e) -> Task<void> {
 *     LOG_INFO << "User created: " << e.username;
 *     co_return;
 * });
 * @endcode
 */
class EventBus {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    /**
     * @brief 发布事件
     */
    template<typename E>
    Task<void> publish(const E& event) {
        static_assert(std::is_base_of_v<DomainEvent, E>, "E must derive from DomainEvent");

        LOG_DEBUG << "EventBus: Publishing " << event.type
                  << " for " << event.aggregateType << "#" << event.aggregateId;

        // 1. 执行内置处理（缓存失效、版本更新）
        // 缓存失效不应阻止事件分发到订阅者
        try {
            co_await handleBuiltinEffects(event);
        } catch (const std::exception& e) {
            LOG_ERROR << "EventBus: handleBuiltinEffects failed for " << event.type
                      << " " << event.aggregateType << "#" << event.aggregateId
                      << ": " << e.what();
        }

        // 2. 执行自定义订阅者
        std::vector<EventHandler> handlers;
        {
            std::shared_lock lock(handlersMutex_);
            auto it = handlers_.find(std::type_index(typeid(event)));
            if (it != handlers_.end()) {
                handlers = it->second;
            }
        }

        for (const auto& handler : handlers) {
            try {
                co_await handler(event);
            } catch (const std::exception& e) {
                LOG_ERROR << "EventBus: Handler failed for " << event.type
                          << ": " << e.what();
            }
        }
    }

    /**
     * @brief 订阅事件
     */
    template<typename E>
    void subscribe(std::function<Task<void>(const E&)> handler) {
        static_assert(std::is_base_of_v<DomainEvent, E>, "E must derive from DomainEvent");

        std::unique_lock lock(handlersMutex_);
        handlers_[std::type_index(typeid(E))].push_back(
            [handler = std::move(handler)](const DomainEvent& e) -> Task<void> {
                co_await handler(static_cast<const E&>(e));
            }
        );
    }

    /**
     * @brief 注销所有事件处理器（服务关闭时调用，防止内存泄漏）
     */
    void unsubscribeAll() {
        std::unique_lock lock(handlersMutex_);
        handlers_.clear();
        LOG_INFO << "EventBus: All handlers unsubscribed";
    }

private:
    EventBus() {
        registerBuiltinEffects();
    }
    mutable std::shared_mutex handlersMutex_;
    std::map<std::type_index, std::vector<EventHandler>> handlers_;
    std::unordered_map<std::string, BuiltinEffectHandler> builtinHandlers_;
    AuthCache authCache_;

    template<typename Handler>
    void registerBuiltinEffect(const std::string& aggregateType, Handler&& handler) {
        builtinHandlers_[aggregateType] = std::forward<Handler>(handler);
    }

    void registerBuiltinEffects() {
        registerBuiltinEffect("User", [this](const DomainEvent& event) -> Task<void> {
            int aggId = event.aggregateId;

            co_await authCache_.clearUserCache(aggId);
            ResourceVersion::instance().incrementVersion("user");

            std::string userEtagKey = "auth:user:" + std::to_string(aggId);
            ResourceVersion::instance().incrementVersion(userEtagKey);

            LOG_DEBUG << "EventBus: Invalidated cache for User#" << aggId;
            co_return;
        });

        registerBuiltinEffect("Role", [this](const DomainEvent& event) -> Task<void> {
            int aggId = event.aggregateId;

            co_await authCache_.clearAllUserRolesCache();
            co_await authCache_.clearAllUserMenusCache();
            co_await authCache_.clearAllUserSessionsCache();
            ResourceVersion::instance().incrementVersion("role");

            LOG_DEBUG << "EventBus: Invalidated all user caches for Role#" << aggId;
            co_return;
        });

        registerBuiltinEffect("Menu", [this](const DomainEvent& event) -> Task<void> {
            int aggId = event.aggregateId;

            co_await authCache_.clearAllUserMenusCache();
            co_await authCache_.clearAllUserSessionsCache();
            ResourceVersion::instance().incrementVersion("menu");

            LOG_DEBUG << "EventBus: Invalidated menu and session caches for Menu#" << aggId;
            co_return;
        });

        registerBuiltinEffect("Department", [](const DomainEvent& event) -> Task<void> {
            ResourceVersion::instance().incrementVersion("department");

            LOG_DEBUG << "EventBus: Updated version for Department#" << event.aggregateId;
            co_return;
        });

        registerBuiltinEffect("Device", [](const DomainEvent& event) -> Task<void> {
            if (event.type == "DeviceCreated"
                || event.type == "DeviceUpdated"
                || event.type == "DeviceDeleted") {
                try {
                    co_await DeviceCache::instance().refreshDevice(event.aggregateId);
                } catch (const std::exception& e) {
                    LOG_WARN << "EventBus: incremental device cache refresh failed for Device#"
                             << event.aggregateId << ": " << e.what()
                             << ", falling back to stale refresh";
                    DeviceCache::instance().markStale();
                }
                RealtimeDataCache::instance().invalidate(event.aggregateId);
                ResourceVersion::instance().incrementVersion("device");

                LOG_DEBUG << "EventBus: Refreshed device cache for Device#" << event.aggregateId;
            }
            co_return;
        });

        registerBuiltinEffect("Link", [](const DomainEvent& event) -> Task<void> {
            try {
                co_await DeviceCache::instance().refreshDevicesByLinkId(event.aggregateId);
            } catch (const std::exception& e) {
                LOG_WARN << "EventBus: incremental device cache refresh failed for Link#"
                         << event.aggregateId << ": " << e.what()
                         << ", falling back to stale refresh";
                DeviceCache::instance().markStale();
            }
            ResourceVersion::instance().incrementVersion("link");

            LOG_DEBUG << "EventBus: Refreshed device cache for Link#" << event.aggregateId;
            co_return;
        });

        registerBuiltinEffect("ProtocolConfig", [](const DomainEvent& event) -> Task<void> {
            try {
                co_await DeviceCache::instance().refreshDevicesByProtocolConfigId(event.aggregateId);
            } catch (const std::exception& e) {
                LOG_WARN << "EventBus: incremental device cache refresh failed for ProtocolConfig#"
                         << event.aggregateId << ": " << e.what()
                         << ", falling back to stale refresh";
                DeviceCache::instance().markStale();
            }
            ResourceVersion::instance().incrementVersion("protocol");

            LOG_DEBUG << "EventBus: Refreshed device cache for ProtocolConfig#"
                      << event.aggregateId;
            co_return;
        });

        registerBuiltinEffect("DeviceGroup", [](const DomainEvent& event) -> Task<void> {
            ResourceVersion::instance().incrementVersion("deviceGroup");

            LOG_DEBUG << "EventBus: Updated version for DeviceGroup#" << event.aggregateId;
            co_return;
        });

        registerBuiltinEffect("AlertRule", [](const DomainEvent& event) -> Task<void> {
            ResourceVersion::instance().incrementVersion("alert");

            LOG_DEBUG << "EventBus: Updated version for AlertRule#" << event.aggregateId;
            co_return;
        });
    }

    /**
     * @brief 内置副作用处理
     * 根据事件类型自动处理缓存失效和资源版本更新
     */
    Task<void> handleBuiltinEffects(const DomainEvent& event) {
        auto it = builtinHandlers_.find(event.aggregateType);
        if (it != builtinHandlers_.end()) {
            co_await it->second(event);
        }
    }
};
