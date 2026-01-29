/**
 * 首页 Query Hooks
 */

import { useQuery, type UseQueryOptions } from "@tanstack/react-query";
import { homeKeys } from "./keys";
import * as api from "./api";
import type { HomeStats, SystemInfo } from "./types";

/**
 * 获取统计数据
 */
export function useHomeStats(options?: Omit<UseQueryOptions<HomeStats>, "queryKey" | "queryFn">) {
  return useQuery({
    queryKey: homeKeys.stats(),
    queryFn: api.getStats,
    staleTime: 30 * 1000, // 30秒缓存
    ...options,
  });
}

/**
 * 获取系统信息
 */
export function useSystemInfo(options?: Omit<UseQueryOptions<SystemInfo>, "queryKey" | "queryFn">) {
  return useQuery({
    queryKey: homeKeys.systemInfo(),
    queryFn: api.getSystemInfo,
    staleTime: 60 * 1000, // 1分钟缓存
    ...options,
  });
}
