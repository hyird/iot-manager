/**
 * 用户管理类型定义
 */

import type { PageParams } from "../common";

// ============ 枚举/状态类型 ============

export type UserStatus = "enabled" | "disabled";

// ============ 基础类型 ============

export interface UserRole {
  id: number;
  name: string;
  code: string;
}

// ============ 列表项/详情类型 ============

export interface UserItem {
  id: number;
  username: string;
  nickname?: string;
  phone?: string;
  email?: string;
  department_id?: number | null;
  department_name?: string;
  status: UserStatus;
  roles: UserRole[];
  created_at?: string;
  updated_at?: string;
}

// ============ 查询参数 ============

export interface UserQuery extends PageParams {
  status?: UserStatus;
  department_id?: number;
}

// ============ DTO 类型 ============

export interface CreateUserDto {
  username: string;
  password: string;
  nickname?: string;
  phone?: string;
  email?: string;
  department_id?: number | null;
  status?: UserStatus;
  role_ids?: number[];
}

export interface UpdateUserDto {
  nickname?: string;
  phone?: string;
  email?: string;
  department_id?: number | null;
  status?: UserStatus;
  password?: string;
  role_ids?: number[];
}

export interface UpdatePasswordDto {
  old_password: string;
  new_password: string;
}
