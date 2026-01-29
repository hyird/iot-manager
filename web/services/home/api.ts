/**
 * 首页 API
 */

import { request } from "../common";
import type { HomeStats, SystemInfo } from "./types";

/** API 端点 */
const ENDPOINTS = {
  STATS: "/api/home/stats",
  SYSTEM: "/api/home/system",
} as const;

/** 获取统计数据 */
export function getStats() {
  return request.get<HomeStats>(ENDPOINTS.STATS);
}

/** 获取系统信息 */
export function getSystemInfo() {
  return request.get<SystemInfo>(ENDPOINTS.SYSTEM);
}
