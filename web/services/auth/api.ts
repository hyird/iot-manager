/**
 * 认证 API
 */

import type { Auth } from "@/types";
import type { RequestConfig } from "../common/request";
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
export function refreshToken(refreshToken: string, config?: RequestConfig) {
  return request.post<{ token: string; refreshToken: string }>(
    ENDPOINTS.REFRESH,
    {
      refreshToken,
    },
    config
  );
}

/** 获取当前用户信息 */
export function fetchCurrentUser(config?: RequestConfig) {
  return request.get<Auth.UserInfo>(ENDPOINTS.ME, config);
}

/** 登出 */
export function logout(refreshToken?: string, config?: RequestConfig) {
  return request.post<void>(ENDPOINTS.LOGOUT, { refreshToken: refreshToken ?? "" }, config);
}
