/**
 * 设备管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import { useMemo } from "react";
import type { Device } from "@/types";
import type { PaginatedResult } from "../common";
import * as deviceApi from "./api";
import { deviceKeys } from "./keys";

/** 设备详情 Query */
export function useDeviceDetail(
  id: number,
  options?: Omit<UseQueryOptions<Device.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: deviceKeys.detail(id),
    queryFn: () => deviceApi.getDetail(id),
    enabled: id > 0,
    ...options,
  });
}

/** 设备选项列表 Query */
export function useDeviceOptions(
  options?: Omit<UseQueryOptions<PaginatedResult<Device.Option>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: deviceKeys.options(),
    queryFn: () => deviceApi.getOptions(),
    ...options,
  });
}

/** 设备静态数据 Query（支持 ETag 缓存，不轮询） */
export function useDeviceStatic(
  options?: Omit<UseQueryOptions<PaginatedResult<Device.StaticData>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: deviceKeys.static(),
    queryFn: () => deviceApi.getStatic(),
    staleTime: 5 * 60 * 1000, // 5分钟
    ...options,
  });
}

/** 设备实时数据 Query（轮询获取） */
export function useDeviceRealtime(
  options?: Omit<UseQueryOptions<PaginatedResult<Device.Realtime>>, "queryKey" | "queryFn"> & {
    /** 轮询间隔（毫秒），默认 3000ms，设为 false 禁用轮询 */
    pollingInterval?: number | false;
  }
) {
  const { pollingInterval = 3000, ...queryOptions } = options || {};
  return useQuery({
    queryKey: deviceKeys.realtime(),
    queryFn: () => deviceApi.getRealtime(),
    refetchInterval: pollingInterval,
    refetchIntervalInBackground: false,
    ...queryOptions,
  });
}

/**
 * 设备列表（合并静态数据和实时数据）
 * 保持与原 useDeviceList 接口兼容
 */
export function useDeviceList(options?: {
  /** 轮询间隔（毫秒），默认 3000ms，设为 false 禁用轮询 */
  pollingInterval?: number | false;
  /** 是否启用查询 */
  enabled?: boolean;
}) {
  const { pollingInterval = 3000, enabled = true } = options || {};

  const staticQuery = useDeviceStatic({ enabled });
  const realtimeQuery = useDeviceRealtime({
    pollingInterval,
    enabled,
  });

  // 合并静态数据和实时数据
  const mergedList = useMemo(() => {
    const staticList = staticQuery.data?.list || [];
    const realtimeList = realtimeQuery.data?.list || [];

    if (staticList.length === 0) return [];

    // 创建实时数据映射
    const realtimeMap = new Map(realtimeList.map((r) => [r.id, r]));

    // 合并数据
    return staticList.map((device) => {
      const realtime = realtimeMap.get(device.id);
      return {
        ...device,
        // 实时数据字段
        reportTime: realtime?.reportTime,
        lastHeartbeatTime: realtime?.lastHeartbeatTime,
        elements: realtime?.elements,
        image: realtime?.image,
      } as Device.RealTimeData;
    });
  }, [staticQuery.data, realtimeQuery.data]);

  return {
    data: { list: mergedList },
    isLoading: staticQuery.isLoading || realtimeQuery.isLoading,
    isError: staticQuery.isError || realtimeQuery.isError,
    error: staticQuery.error || realtimeQuery.error,
    refetch: () => {
      staticQuery.refetch();
      realtimeQuery.refetch();
    },
  };
}

/** 设备历史数据 Query */
export function useDeviceHistoryData(
  params: Device.HistoryQuery,
  options?: Omit<
    UseQueryOptions<
      PaginatedResult<
        Device.HistoryDevice | Device.HistoryFunc | Device.HistoryElement | Device.HistoryImage
      >
    >,
    "queryKey" | "queryFn"
  >
) {
  return useQuery({
    queryKey: deviceKeys.history(params),
    queryFn: () => deviceApi.getHistoryData(params),
    ...options,
  });
}
