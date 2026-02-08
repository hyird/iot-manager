#pragma once

/**
 * @brief 领域事件基类
 *
 * 领域事件代表领域中发生的重要业务事件。
 * 用于解耦聚合根与副作用（如缓存失效、通知等）。
 *
 * 具体事件定义在各模块的 domain/Events.hpp 中。
 */
struct DomainEvent {
    std::string type;                    // 事件类型标识
    int aggregateId = 0;                 // 聚合根 ID
    std::string aggregateType;           // 聚合根类型
    std::chrono::system_clock::time_point occurredAt;  // 发生时间

    DomainEvent() : occurredAt(std::chrono::system_clock::now()) {}

    DomainEvent(std::string eventType, int aggId, std::string aggType)
        : type(std::move(eventType))
        , aggregateId(aggId)
        , aggregateType(std::move(aggType))
        , occurredAt(std::chrono::system_clock::now()) {}

    virtual ~DomainEvent() = default;

    // 允许拷贝和移动（事件对象需要在订阅者间传递）
    DomainEvent(const DomainEvent&) = default;
    DomainEvent& operator=(const DomainEvent&) = default;
    DomainEvent(DomainEvent&&) = default;
    DomainEvent& operator=(DomainEvent&&) = default;
};
