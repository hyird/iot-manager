/**
 * 系统模块 UI 常量
 * 用于下拉选项、状态映射等
 */

import type { DepartmentStatus, MenuStatus, MenuType, RoleStatus, UserStatus } from "../system";

// ==================== 菜单模块 ====================

export const MenuTypeOptions = [
  { label: "菜单", value: "menu" },
  { label: "页面", value: "page" },
  { label: "按钮", value: "button" },
] as const;

export const MenuTypeMap: Record<MenuType, { text: string; color: string }> = {
  menu: { text: "菜单", color: "blue" },
  page: { text: "页面", color: "green" },
  button: { text: "按钮", color: "purple" },
};

export const MenuStatusOptions = [
  { label: "启用", value: "enabled" },
  { label: "禁用", value: "disabled" },
] as const;

export const MenuStatusMap: Record<MenuStatus, { text: string; color: string }> = {
  enabled: { text: "启用", color: "success" },
  disabled: { text: "禁用", color: "default" },
};

// ==================== 用户模块 ====================

export const UserStatusOptions = [
  { label: "启用", value: "enabled" },
  { label: "禁用", value: "disabled" },
] as const;

export const UserStatusMap: Record<UserStatus, { text: string; color: string }> = {
  enabled: { text: "启用", color: "success" },
  disabled: { text: "禁用", color: "default" },
};

// ==================== 角色模块 ====================

export const RoleStatusOptions = [
  { label: "启用", value: "enabled" },
  { label: "禁用", value: "disabled" },
] as const;

export const RoleStatusMap: Record<RoleStatus, { text: string; color: string }> = {
  enabled: { text: "启用", color: "success" },
  disabled: { text: "禁用", color: "default" },
};

// ==================== 部门模块 ====================

export const DepartmentStatusOptions = [
  { label: "启用", value: "enabled" },
  { label: "禁用", value: "disabled" },
] as const;

export const DepartmentStatusMap: Record<DepartmentStatus, { text: string; color: string }> = {
  enabled: { text: "启用", color: "success" },
  disabled: { text: "禁用", color: "default" },
};
