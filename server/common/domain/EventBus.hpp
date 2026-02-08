#pragma once

#include "DomainEvent.hpp"
#include "common/cache/AuthCache.hpp"
#include "common/cache/ResourceVersion.hpp"
#include "common/cache/DeviceCache.hpp"

/**
 * @brief 事件处理器类型
 */
using EventHandler = std::function<drogon::Task<void>(const DomainEvent&)>;

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
        // 即使 Redis 不可用也不应阻止事件分发到订阅者
        try {
            co_await handleBuiltinEffects(event);
        } catch (const std::exception& e) {
            LOG_ERROR << "EventBus: handleBuiltinEffects failed for " << event.type
                      << " " << event.aggregateType << "#" << event.aggregateId
                      << ": " << e.what();
        }

        // 2. 执行自定义订阅者
        auto it = handlers_.find(std::type_index(typeid(event)));
        if (it != handlers_.end()) {
            for (const auto& handler : it->second) {
                try {
                    co_await handler(event);
                } catch (const std::exception& e) {
                    LOG_ERROR << "EventBus: Handler failed for " << event.type
                              << ": " << e.what();
                }
            }
        }
    }

    /**
     * @brief 订阅事件
     */
    template<typename E>
    void subscribe(std::function<Task<void>(const E&)> handler) {
        static_assert(std::is_base_of_v<DomainEvent, E>, "E must derive from DomainEvent");

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
        handlers_.clear();
        LOG_INFO << "EventBus: All handlers unsubscribed";
    }

private:
    EventBus() = default;
    std::map<std::type_index, std::vector<EventHandler>> handlers_;
    AuthCache authCache_;

    /**
     * @brief 内置副作用处理
     * 根据事件类型自动处理缓存失效和资源版本更新
     */
    Task<void> handleBuiltinEffects(const DomainEvent& event) {
        const auto& aggType = event.aggregateType;
        int aggId = event.aggregateId;

        // ========== User 事件 ==========
        if (aggType == "User") {
            // 清除该用户的缓存
            co_await authCache_.clearUserCache(aggId);
            ResourceVersion::instance().incrementVersion("user");

            // 失效该用户的 ETag 版本（用于 /api/auth/me）
            std::string userEtagKey = "auth:user:" + std::to_string(aggId);
            ResourceVersion::instance().incrementVersion(userEtagKey);

            LOG_DEBUG << "EventBus: Invalidated cache for User#" << aggId;
        }

        // ========== Role 事件 ==========
        else if (aggType == "Role") {
            // 角色变更影响所有用户
            co_await authCache_.clearAllUserRolesCache();
            co_await authCache_.clearAllUserMenusCache();
            co_await authCache_.clearAllUserSessionsCache();
            ResourceVersion::instance().incrementVersion("role");

            LOG_DEBUG << "EventBus: Invalidated all user caches for Role#" << aggId;
        }

        // ========== Menu 事件 ==========
        else if (aggType == "Menu") {
            // 菜单变更影响所有用户菜单和会话缓存
            co_await authCache_.clearAllUserMenusCache();
            co_await authCache_.clearAllUserSessionsCache();
            ResourceVersion::instance().incrementVersion("menu");

            LOG_DEBUG << "EventBus: Invalidated menu and session caches for Menu#" << aggId;
        }

        // ========== Department 事件 ==========
        else if (aggType == "Department") {
            ResourceVersion::instance().incrementVersion("department");

            LOG_DEBUG << "EventBus: Updated version for Department#" << aggId;
        }

        // ========== Device 事件 ==========
        else if (aggType == "Device") {
            DeviceCache::instance().markStale();
            ResourceVersion::instance().incrementVersion("device");

            LOG_DEBUG << "EventBus: Invalidated device cache for Device#" << aggId;
        }

        // ========== Link 事件 ==========
        else if (aggType == "Link") {
            DeviceCache::instance().markStale();
            ResourceVersion::instance().incrementVersion("link");

            LOG_DEBUG << "EventBus: Invalidated device cache for Link#" << aggId;
        }

        // ========== ProtocolConfig 事件 ==========
        else if (aggType == "ProtocolConfig") {
            DeviceCache::instance().markStale();
            ResourceVersion::instance().incrementVersion("protocol");

            LOG_DEBUG << "EventBus: Invalidated device cache for ProtocolConfig#" << aggId;
        }

        // ========== DeviceGroup 事件 ==========
        else if (aggType == "DeviceGroup") {
            ResourceVersion::instance().incrementVersion("deviceGroup");

            LOG_DEBUG << "EventBus: Updated version for DeviceGroup#" << aggId;
        }

        // ========== AlertRule 事件 ==========
        else if (aggType == "AlertRule") {
            ResourceVersion::instance().incrementVersion("alert");

            LOG_DEBUG << "EventBus: Updated version for AlertRule#" << aggId;
        }
    }
};
