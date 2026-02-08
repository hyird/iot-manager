/**
 * 首页 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import * as api from "./api";
import { homeKeys } from "./keys";
import type { HomeStats, MonitorData, SystemInfo } from "./types";

/**
 * 获取统计数据
 */
export function useHomeStats(options?: Omit<UseQueryOptions<HomeStats>, "queryKey" | "queryFn">) {
  return useQuery({
    queryKey: homeKeys.stats(),
    queryFn: api.getStats,
    staleTime: 15 * 1000,
    refetchInterval: 30 * 1000,
    refetchIntervalInBackground: false,
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
    staleTime: 60 * 1000,
    ...options,
  });
}

/**
 * 获取监控数据（30秒自动刷新）
 */
export function useMonitorData(
  options?: Omit<UseQueryOptions<MonitorData>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: homeKeys.monitor(),
    queryFn: api.getMonitor,
    staleTime: 15 * 1000,
    refetchInterval: 30 * 1000,
    refetchIntervalInBackground: false,
    ...options,
  });
}
