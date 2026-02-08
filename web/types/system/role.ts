/**
 * 角色管理类型定义
 */

import type { PageParams } from "../common";
import type { MenuType } from "./menu";

// ============ 枚举/状态类型 ============

export type RoleStatus = "enabled" | "disabled";

// ============ 列表项/详情类型 ============

export interface RoleItem {
  id: number;
  name: string;
  code: string;
  status: RoleStatus;
  menu_ids?: number[];
}

export interface RoleDetail extends RoleItem {
  menu_ids: number[];
  menus: {
    id: number;
    name: string;
    type: MenuType;
    parent_id: number | null;
  }[];
}

export interface RoleOption {
  id: number;
  name: string;
  code: string;
}

// ============ 查询参数 ============

export interface RoleQuery extends PageParams {
  status?: RoleStatus;
}

// ============ DTO 类型 ============

export interface CreateRoleDto {
  name: string;
  code: string;
  status?: RoleStatus;
  menu_ids?: number[];
}

export interface UpdateRoleDto {
  name?: string;
  code?: string;
  status?: RoleStatus;
  menu_ids?: number[];
}
