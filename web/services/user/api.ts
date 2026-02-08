/**
 * 用户管理 API
 */

import type { User } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/users",
  DETAIL: (id: number) => `/api/users/${id}`,
} as const;

/** 获取用户列表 */
export function getList(params?: User.Query) {
  return request.get<PaginatedResult<User.Item>>(ENDPOINTS.BASE, { params });
}

/** 获取用户详情 */
export function getDetail(id: number) {
  return request.get<User.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建用户 */
export function create(data: User.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新用户 */
export function update(id: number, data: User.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除用户 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}
