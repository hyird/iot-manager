/**
 * 角色管理 API
 */

import type { Role } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/roles",
  DETAIL: (id: number) => `/api/roles/${id}`,
  ALL: "/api/roles/all",
} as const;

/** 获取角色列表 */
export function getList(params?: Role.Query) {
  return request.get<PaginatedResult<Role.Item>>(ENDPOINTS.BASE, { params });
}

/** 获取角色详情 */
export function getDetail(id: number) {
  return request.get<Role.Detail>(ENDPOINTS.DETAIL(id));
}

/** 获取所有启用角色（下拉用） */
export function getAll() {
  return request.get<Role.Option[]>(ENDPOINTS.ALL);
}

/** 创建角色 */
export function create(data: Role.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新角色 */
export function update(id: number, data: Role.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除角色 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}
