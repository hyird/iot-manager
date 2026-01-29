#pragma once

#include <string>
#include <chrono>
#include <memory>

/**
 * @brief 领域事件基类
 *
 * 领域事件代表领域中发生的重要业务事件。
 * 用于解耦聚合根与副作用（如缓存失效、通知等）。
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

    // 禁止拷贝，允许移动
    DomainEvent(const DomainEvent&) = default;
    DomainEvent& operator=(const DomainEvent&) = default;
    DomainEvent(DomainEvent&&) = default;
    DomainEvent& operator=(DomainEvent&&) = default;
};

// ==================== 用户相关事件 ====================

struct UserCreated : DomainEvent {
    std::string username;

    UserCreated(int userId, std::string name)
        : DomainEvent("UserCreated", userId, "User")
        , username(std::move(name)) {}
};

struct UserUpdated : DomainEvent {
    UserUpdated(int userId)
        : DomainEvent("UserUpdated", userId, "User") {}
};

struct UserDeleted : DomainEvent {
    UserDeleted(int userId)
        : DomainEvent("UserDeleted", userId, "User") {}
};

struct UserRolesChanged : DomainEvent {
    std::vector<int> roleIds;

    UserRolesChanged(int userId, std::vector<int> roles)
        : DomainEvent("UserRolesChanged", userId, "User")
        , roleIds(std::move(roles)) {}
};

// ==================== 角色相关事件 ====================

struct RoleCreated : DomainEvent {
    std::string code;

    RoleCreated(int roleId, std::string roleCode)
        : DomainEvent("RoleCreated", roleId, "Role")
        , code(std::move(roleCode)) {}
};

struct RoleUpdated : DomainEvent {
    RoleUpdated(int roleId)
        : DomainEvent("RoleUpdated", roleId, "Role") {}
};

struct RoleDeleted : DomainEvent {
    RoleDeleted(int roleId)
        : DomainEvent("RoleDeleted", roleId, "Role") {}
};

struct RoleMenusChanged : DomainEvent {
    RoleMenusChanged(int roleId)
        : DomainEvent("RoleMenusChanged", roleId, "Role") {}
};

// ==================== 部门相关事件 ====================

struct DepartmentCreated : DomainEvent {
    DepartmentCreated(int deptId)
        : DomainEvent("DepartmentCreated", deptId, "Department") {}
};

struct DepartmentUpdated : DomainEvent {
    DepartmentUpdated(int deptId)
        : DomainEvent("DepartmentUpdated", deptId, "Department") {}
};

struct DepartmentDeleted : DomainEvent {
    DepartmentDeleted(int deptId)
        : DomainEvent("DepartmentDeleted", deptId, "Department") {}
};

// ==================== 菜单相关事件 ====================

struct MenuCreated : DomainEvent {
    MenuCreated(int menuId)
        : DomainEvent("MenuCreated", menuId, "Menu") {}
};

struct MenuUpdated : DomainEvent {
    MenuUpdated(int menuId)
        : DomainEvent("MenuUpdated", menuId, "Menu") {}
};

struct MenuDeleted : DomainEvent {
    MenuDeleted(int menuId)
        : DomainEvent("MenuDeleted", menuId, "Menu") {}
};

// ==================== 设备相关事件 ====================

struct DeviceCreated : DomainEvent {
    std::string deviceCode;

    DeviceCreated(int deviceId, std::string code)
        : DomainEvent("DeviceCreated", deviceId, "Device")
        , deviceCode(std::move(code)) {}
};

struct DeviceUpdated : DomainEvent {
    DeviceUpdated(int deviceId)
        : DomainEvent("DeviceUpdated", deviceId, "Device") {}
};

struct DeviceDeleted : DomainEvent {
    DeviceDeleted(int deviceId)
        : DomainEvent("DeviceDeleted", deviceId, "Device") {}
};

// ==================== 链路相关事件 ====================

struct LinkCreated : DomainEvent {
    std::string name;
    std::string mode;
    std::string ip;
    int port;

    LinkCreated(int linkId, std::string n, std::string m, std::string i, int p)
        : DomainEvent("LinkCreated", linkId, "Link")
        , name(std::move(n)), mode(std::move(m)), ip(std::move(i)), port(p) {}
};

struct LinkUpdated : DomainEvent {
    std::string name;
    std::string mode;
    std::string ip;
    int port;
    bool enabled;
    bool needReload;

    LinkUpdated(int linkId, std::string n, std::string m, std::string i, int p, bool e, bool reload)
        : DomainEvent("LinkUpdated", linkId, "Link")
        , name(std::move(n)), mode(std::move(m)), ip(std::move(i))
        , port(p), enabled(e), needReload(reload) {}
};

struct LinkDeleted : DomainEvent {
    LinkDeleted(int linkId)
        : DomainEvent("LinkDeleted", linkId, "Link") {}
};

// ==================== 协议配置相关事件 ====================

struct ProtocolConfigCreated : DomainEvent {
    std::string protocol;
    std::string name;

    ProtocolConfigCreated(int configId, std::string p, std::string n)
        : DomainEvent("ProtocolConfigCreated", configId, "ProtocolConfig")
        , protocol(std::move(p)), name(std::move(n)) {}
};

struct ProtocolConfigUpdated : DomainEvent {
    ProtocolConfigUpdated(int configId)
        : DomainEvent("ProtocolConfigUpdated", configId, "ProtocolConfig") {}
};

struct ProtocolConfigDeleted : DomainEvent {
    ProtocolConfigDeleted(int configId)
        : DomainEvent("ProtocolConfigDeleted", configId, "ProtocolConfig") {}
};
