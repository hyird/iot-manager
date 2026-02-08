#pragma once

#include "common/domain/DomainEvent.hpp"

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
