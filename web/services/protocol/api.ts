/**
 * 协议配置 API
 */

import type { Protocol } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/protocol/configs",
  DETAIL: (id: number) => `/api/protocol/configs/${id}`,
  OPTIONS: "/api/protocol/configs/options",
} as const;

/** 获取配置列表 */
export function getList(params?: Protocol.Query) {
  return request.get<PaginatedResult<Protocol.Item>>(ENDPOINTS.BASE, { params });
}

/** 获取配置详情 */
export function getDetail(id: number) {
  return request.get<Protocol.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建配置 */
export function create(data: Protocol.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新配置 */
export function update(id: number, data: Protocol.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除配置 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}

/** 获取指定协议的配置选项列表 */
export function getOptions(protocol: Protocol.Type) {
  return request.get<PaginatedResult<Protocol.Option>>(ENDPOINTS.OPTIONS, {
    params: { protocol },
  });
}
