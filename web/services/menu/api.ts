/**
 * 菜单管理 API
 */

import type { Menu } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/menus",
  DETAIL: (id: number) => `/api/menus/${id}`,
  TREE: "/api/menus/tree",
} as const;

/** 获取菜单列表 */
export function getList(params?: Menu.Query) {
  return request.get<PaginatedResult<Menu.Item>>(ENDPOINTS.BASE, { params });
}

/** 获取菜单树 */
export function getTree(status?: Menu.Status) {
  return request.get<Menu.TreeItem[]>(ENDPOINTS.TREE, {
    params: status ? { status } : undefined,
  });
}

/** 获取菜单详情 */
export function getDetail(id: number) {
  return request.get<Menu.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建菜单 */
export function create(data: Menu.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新菜单 */
export function update(id: number, data: Menu.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除菜单 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}
