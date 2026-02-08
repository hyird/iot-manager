/**
 * 设备管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import { useCallback, useMemo, useRef } from "react";
import type { Device } from "@/types";
import { deepEqual } from "@/utils/deepEqual";
import type { PaginatedResult } from "../common";
import * as deviceApi from "./api";
import { deviceKeys } from "./keys";

const areImagesEqual = (
  previous?: Device.Realtime["image"],
  current?: Device.Realtime["image"]
) => {
  if (previous === current) return true;
  if (!previous || !current) return previous === current;
  return previous.data === current.data;
};

const areElementsEqual = (
  previous?: Device.Realtime["elements"],
  current?: Device.Realtime["elements"]
) => {
  if (previous === current) return true;
  if (!previous || !current) return previous === current;
  if (previous.length !== current.length) return false;

  return previous.every((previousElement, index) => {
    const currentElement = current[index];
    if (previousElement === currentElement) return true;

    return (
      previousElement.name === currentElement.name &&
      previousElement.value === currentElement.value &&
      previousElement.unit === currentElement.unit &&
      previousElement.encode === currentElement.encode &&
      (previousElement.dictConfig === currentElement.dictConfig ||
        deepEqual(previousElement.dictConfig, currentElement.dictConfig))
    );
  });
};

const areRealtimeEntriesEqual = (previous?: Device.Realtime, current?: Device.Realtime) => {
  if (previous === current) return true;
  if (!previous || !current) return previous === current;

  return (
    previous.reportTime === current.reportTime &&
    previous.connected === current.connected &&
    areImagesEqual(previous.image, current.image) &&
    areElementsEqual(previous.elements, current.elements)
  );
};

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
  type MergedCacheEntry = {
    staticRef: Device.StaticData;
    realtimeRef?: Device.Realtime;
    merged: Device.RealTimeData;
  };
  const mergedCacheRef = useRef<Map<number, MergedCacheEntry>>(new Map());
  const mergedListRef = useRef<Device.RealTimeData[]>([]);

  const staticQuery = useDeviceStatic({ enabled });
  const realtimeQuery = useDeviceRealtime({
    pollingInterval,
    enabled,
  });
  const refetchStatic = staticQuery.refetch;
  const refetchRealtime = realtimeQuery.refetch;

  // 合并静态数据和实时数据
  const mergedList = useMemo(() => {
    const staticList = staticQuery.data?.list ?? [];
    const realtimeList = realtimeQuery.data?.list ?? [];

    if (staticList.length === 0) {
      mergedCacheRef.current.clear();
      mergedListRef.current = [];
      return [];
    }

    // 创建实时数据映射
    const realtimeMap = new Map(realtimeList.map((r) => [r.id, r]));
    const nextCache = new Map<number, MergedCacheEntry>();

    // 合并数据：保持未变化设备对象引用稳定，减少卡片重渲染
    const nextList = staticList.map((device) => {
      const realtime = realtimeMap.get(device.id);
      const cachedEntry = mergedCacheRef.current.get(device.id);
      const isStaticStable =
        cachedEntry?.staticRef === device || deepEqual(cachedEntry?.staticRef, device);
      const isRealtimeStable = areRealtimeEntriesEqual(cachedEntry?.realtimeRef, realtime);

      if (cachedEntry && isStaticStable && isRealtimeStable) {
        nextCache.set(device.id, cachedEntry);
        return cachedEntry.merged;
      }

      const merged = {
        ...device,
        // 实时数据字段
        reportTime: realtime?.reportTime,
        connected: realtime?.connected,
        elements: realtime?.elements,
        image: realtime?.image,
      } as Device.RealTimeData;

      nextCache.set(device.id, {
        staticRef: device,
        realtimeRef: realtime,
        merged,
      });

      return merged;
    });

    mergedCacheRef.current = nextCache;
    const previousList = mergedListRef.current;
    const shouldReuseList =
      previousList.length === nextList.length &&
      previousList.every((item, index) => item === nextList[index]);

    if (shouldReuseList) {
      return previousList;
    }

    mergedListRef.current = nextList;
    return nextList;
  }, [staticQuery.data?.list, realtimeQuery.data?.list]);

  const data = useMemo(() => ({ list: mergedList }), [mergedList]);
  const refetch = useCallback(
    () => Promise.all([refetchStatic(), refetchRealtime()]),
    [refetchRealtime, refetchStatic]
  );

  return useMemo(
    () => ({
      data,
      isLoading: staticQuery.isLoading || realtimeQuery.isLoading,
      isError: staticQuery.isError || realtimeQuery.isError,
      error: staticQuery.error || realtimeQuery.error,
      refetch,
    }),
    [
      data,
      realtimeQuery.error,
      realtimeQuery.isError,
      realtimeQuery.isLoading,
      refetch,
      staticQuery.error,
      staticQuery.isError,
      staticQuery.isLoading,
    ]
  );
}

/** 设备历史数据 Query */
export function useDeviceHistoryData(
  params: Device.HistoryQuery,
  options?: Omit<
    UseQueryOptions<
      PaginatedResult<Device.HistoryElement | Device.HistoryImage>
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
