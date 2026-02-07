/**
 * 部门管理 API
 */

import type { Department } from "@/types";
import { request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/departments",
  DETAIL: (id: number) => `/api/departments/${id}`,
  TREE: "/api/departments/tree",
} as const;

/** 获取部门列表 */
export function getList(params?: Department.Query) {
  return request.get<Department.Item[]>(ENDPOINTS.BASE, { params });
}

/** 获取部门树 */
export function getTree(status?: Department.Status) {
  return request.get<Department.TreeItem[]>(ENDPOINTS.TREE, {
    params: status ? { status } : undefined,
  });
}

/** 获取部门详情 */
export function getDetail(id: number) {
  return request.get<Department.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建部门 */
export function create(data: Department.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新部门 */
export function update(id: number, data: Department.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除部门 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}
