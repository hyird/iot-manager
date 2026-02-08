/**
 * 首页 API
 */

import { request } from "../common";
import type { HomeStats, MonitorData, SystemInfo } from "./types";

/** API 端点 */
const ENDPOINTS = {
  STATS: "/api/home/stats",
  SYSTEM: "/api/home/system",
  MONITOR: "/api/home/monitor",
} as const;

/** 获取统计数据 */
export function getStats() {
  return request.get<HomeStats>(ENDPOINTS.STATS);
}

/** 获取系统信息 */
export function getSystemInfo() {
  return request.get<SystemInfo>(ENDPOINTS.SYSTEM);
}

/** 获取监控数据 */
export function getMonitor() {
  return request.get<MonitorData>(ENDPOINTS.MONITOR);
}

/** 清理所有缓存 */
export function clearCache() {
  return request.post<string>("/api/home/cache/clear");
}
