/**
 * 链路管理 API
 */

import { request, type PaginatedResult } from "../common";
import type { Link } from "@/types";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/link",
  DETAIL: (id: number) => `/api/link/${id}`,
  OPTIONS: "/api/link/options",
  ENUMS: "/api/link/enums",
} as const;

/** 获取链路列表 */
export function getList(params?: Link.Query) {
  return request.get<PaginatedResult<Link.Item>>(ENDPOINTS.BASE, { params });
}

/** 获取链路详情 */
export function getDetail(id: number) {
  return request.get<Link.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建链路 */
export function create(data: Link.CreateDto) {
  return request.post<Link.Item>(ENDPOINTS.BASE, data);
}

/** 更新链路 */
export function update(id: number, data: Link.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除链路 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}

/** 获取链路选项列表 */
export function getOptions() {
  return request.get<Link.Option[]>(ENDPOINTS.OPTIONS);
}

/** 获取链路枚举值（模式和协议列表） */
export function getEnums() {
  return request.get<Link.Enums>(ENDPOINTS.ENUMS);
}
