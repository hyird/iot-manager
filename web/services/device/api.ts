/**
 * 设备管理 API
 */

import type { Device } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  BASE: "/api/device",
  REALTIME: "/api/device/realtime",
  DETAIL: (id: number) => `/api/device/${id}`,
  OPTIONS: "/api/device/options",
  HISTORY: "/api/device/history",
  COMMAND: (linkId: number) => `/api/device/command/${linkId}`,
} as const;

/** 获取设备详情 */
export function getDetail(id: number) {
  return request.get<Device.Item>(ENDPOINTS.DETAIL(id));
}

/** 创建设备 */
export function create(data: Device.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

/** 更新设备 */
export function update(id: number, data: Device.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

/** 删除设备 */
export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}

/** 获取设备选项列表 */
export function getOptions() {
  return request.get<PaginatedResult<Device.Option>>(ENDPOINTS.OPTIONS);
}

/** 获取设备静态数据列表（支持 ETag 缓存） */
export function getStatic() {
  return request.get<PaginatedResult<Device.StaticData>>(ENDPOINTS.BASE);
}

/** 获取设备实时数据列表（用于轮询） */
export function getRealtime() {
  return request.get<PaginatedResult<Device.Realtime>>(ENDPOINTS.REALTIME);
}

/**
 * 获取设备列表（包含实时数据）
 * @deprecated 请使用 getStatic() + getRealtime() 组合
 */
export function getList() {
  return request.get<PaginatedResult<Device.RealTimeData>>(ENDPOINTS.BASE);
}

/** 获取设备历史数据（支持多层查询） */
export function getHistoryData(params: Device.HistoryQuery) {
  return request.get<
    PaginatedResult<
      Device.HistoryDevice | Device.HistoryFunc | Device.HistoryElement | Device.HistoryImage
    >
  >(ENDPOINTS.HISTORY, { params });
}

/** 下发设备指令 */
export function sendCommand(linkId: number, payload: Device.Command) {
  return request.post<void>(ENDPOINTS.COMMAND(linkId), payload);
}
