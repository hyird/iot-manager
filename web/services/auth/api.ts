/**
 * 认证 API
 */

import type { Auth } from "@/types";
import { request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  LOGIN: "/api/auth/login",
  REFRESH: "/api/auth/refresh",
  ME: "/api/auth/me",
  LOGOUT: "/api/auth/logout",
} as const;

/** 登录 */
export function login(params: Auth.LoginRequest) {
  return request.post<Auth.LoginResult>(ENDPOINTS.LOGIN, params);
}

/** 刷新 Token */
export function refreshToken(refreshToken: string) {
  return request.post<{ token: string; refreshToken: string }>(ENDPOINTS.REFRESH, {
    refreshToken,
  });
}

/** 获取当前用户信息 */
export function fetchCurrentUser() {
  return request.get<Auth.UserInfo>(ENDPOINTS.ME);
}

/** 登出 */
export function logout(refreshToken?: string) {
  return request.post<void>(ENDPOINTS.LOGOUT, { refreshToken: refreshToken ?? "" });
}
