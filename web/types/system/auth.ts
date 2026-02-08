/**
 * 认证相关类型定义
 */

import type { MenuItem } from "./menu";
import type { RoleOption } from "./role";

// ============ JWT 相关 ============

export interface JwtPayload {
  user_id: number;
  username: string;
  iat?: number;
  exp?: number;
}

// ============ 登录相关 ============

export interface LoginRequest {
  username: string;
  password: string;
}

export interface LoginResult {
  token: string;
  refreshToken: string;
  user: UserInfo;
}

// ============ 用户信息 ============

export interface UserInfo {
  id: number;
  username: string;
  nickname?: string;
  status: string;
  roles: RoleOption[];
  menus: MenuItem[];
}
