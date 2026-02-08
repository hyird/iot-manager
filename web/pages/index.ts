/**
 * 页面注册入口
 * 集中注册所有页面，保持懒加载
 */

import { registerPage } from "@/config/registry";

// 首页
registerPage({
  component: "Home",
  name: "首页",
  module: "首页",
  description: "系统概览和数据统计",
  loader: () => import("./Home"),
  permissions: [
    {
      code: "home:dashboard:query",
      name: "查看统计",
      description: "查看首页统计数据和系统信息",
      action: "query",
    },
  ],
});

// 用户管理
registerPage({
  component: "User",
  name: "用户管理",
  module: "系统管理",
  description: "系统用户的增删改查、角色分配",
  loader: () => import("./system/User"),
  permissions: [
    { code: "system:user:query", name: "查询用户", action: "query" },
    { code: "system:user:add", name: "新增用户", action: "add" },
    { code: "system:user:edit", name: "编辑用户", action: "edit" },
    { code: "system:user:delete", name: "删除用户", action: "delete" },
  ],
});

// 角色管理
registerPage({
  component: "Role",
  name: "角色管理",
  module: "系统管理",
  description: "角色的增删改查、权限分配",
  loader: () => import("./system/Role"),
  permissions: [
    { code: "system:role:query", name: "查询角色", action: "query" },
    { code: "system:role:add", name: "新增角色", action: "add" },
    { code: "system:role:edit", name: "编辑角色", action: "edit" },
    { code: "system:role:delete", name: "删除角色", action: "delete" },
    { code: "system:role:perm", name: "分配权限", action: "perm" },
  ],
});

// 部门管理
registerPage({
  component: "Dept",
  name: "部门管理",
  module: "系统管理",
  description: "组织架构的树形管理",
  loader: () => import("./system/Department"),
  permissions: [
    { code: "system:dept:query", name: "查询部门", action: "query" },
    { code: "system:dept:add", name: "新增部门", action: "add" },
    { code: "system:dept:edit", name: "编辑部门", action: "edit" },
    { code: "system:dept:delete", name: "删除部门", action: "delete" },
  ],
});

// 菜单管理
registerPage({
  component: "Menu",
  name: "菜单管理",
  module: "系统管理",
  description: "菜单和权限按钮的配置",
  loader: () => import("./system/Menu"),
  permissions: [
    { code: "system:menu:query", name: "查询菜单", action: "query" },
    { code: "system:menu:add", name: "新增菜单", action: "add" },
    { code: "system:menu:edit", name: "编辑菜单", action: "edit" },
    { code: "system:menu:delete", name: "删除菜单", action: "delete" },
  ],
});

// 链路管理
registerPage({
  component: "Link",
  name: "链路管理",
  module: "IOT管理",
  description: "TCP链路的配置和管理",
  loader: () => import("./link/Link"),
  permissions: [
    { code: "iot:link:query", name: "查询链路", action: "query" },
    { code: "iot:link:add", name: "新增链路", action: "add" },
    { code: "iot:link:edit", name: "编辑链路", action: "edit" },
    { code: "iot:link:delete", name: "删除链路", action: "delete" },
  ],
});

// SL651 协议配置
registerPage({
  component: "SL651Config",
  name: "SL651配置",
  module: "IOT管理",
  description: "SL651协议设备类型、功能码、要素的配置",
  loader: () => import("./protocol/SL651Config"),
  permissions: [
    { code: "iot:protocol:query", name: "查询配置", action: "query" },
    { code: "iot:protocol:add", name: "新增配置", action: "add" },
    { code: "iot:protocol:edit", name: "编辑配置", action: "edit" },
    { code: "iot:protocol:delete", name: "删除配置", action: "delete" },
    { code: "iot:protocol:import", name: "导入配置", action: "import" },
    { code: "iot:protocol:export", name: "导出配置", action: "export" },
  ],
});

// Modbus 协议配置
registerPage({
  component: "ModbusConfig",
  name: "Modbus配置",
  module: "IOT管理",
  description: "Modbus协议设备类型、寄存器配置",
  loader: () => import("./protocol/ModbusConfig"),
  permissions: [
    { code: "iot:protocol:query", name: "查询配置", action: "query" },
    { code: "iot:protocol:add", name: "新增配置", action: "add" },
    { code: "iot:protocol:edit", name: "编辑配置", action: "edit" },
    { code: "iot:protocol:delete", name: "删除配置", action: "delete" },
    { code: "iot:protocol:import", name: "导入配置", action: "import" },
    { code: "iot:protocol:export", name: "导出配置", action: "export" },
  ],
});

// 设备管理
registerPage({
  component: "Device",
  name: "设备管理",
  module: "IOT管理",
  description: "设备的增删改查、链路和协议绑定",
  loader: () => import("./device/Device"),
  permissions: [
    { code: "iot:device:query", name: "查询设备", action: "query" },
    { code: "iot:device:add", name: "新增设备", action: "add" },
    { code: "iot:device:edit", name: "编辑设备", action: "edit" },
    { code: "iot:device:delete", name: "删除设备", action: "delete" },
    { code: "iot:device-group:query", name: "查询分组", action: "query" },
    { code: "iot:device-group:add", name: "新增分组", action: "add" },
    { code: "iot:device-group:edit", name: "编辑分组", action: "edit" },
    { code: "iot:device-group:delete", name: "删除分组", action: "delete" },
  ],
});

// 告警管理
registerPage({
  component: "Alert",
  name: "告警管理",
  module: "告警管理",
  description: "告警规则的配置和告警记录管理",
  loader: () => import("./alert/Alert"),
  permissions: [
    { code: "iot:alert:query", name: "查询", action: "query" },
    { code: "iot:alert:add", name: "新增规则", action: "add" },
    { code: "iot:alert:edit", name: "编辑规则", action: "edit" },
    { code: "iot:alert:delete", name: "删除规则", action: "delete" },
    { code: "iot:alert:ack", name: "确认告警", action: "ack" },
  ],
});
