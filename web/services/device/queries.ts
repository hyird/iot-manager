/**
 * 设备管理 Query Hooks
 */

import { type UseQueryOptions, useQuery, useQueryClient } from "@tanstack/react-query";
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
      previousElement.group === currentElement.group &&
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
    previous.connectionState === current.connectionState &&
    areImagesEqual(previous.image, current.image) &&
    areElementsEqual(previous.elements, current.elements)
  );
};

const getReportTimeMs = (reportTime?: string) => {
  if (!reportTime) return Number.NEGATIVE_INFINITY;
  const ts = new Date(reportTime).getTime();
  return Number.isNaN(ts) ? Number.NEGATIVE_INFINITY : ts;
};

const shouldPreferCurrentRealtime = (current?: Device.Realtime, fetched?: Device.Realtime) => {
  if (!current) return false;
  if (!fetched) return true;

  const currentTs = getReportTimeMs(current.reportTime);
  const fetchedTs = getReportTimeMs(fetched.reportTime);

  if (currentTs !== fetchedTs) {
    return currentTs > fetchedTs;
  }

  if (current.connectionState !== fetched.connectionState) {
    if (current.connectionState === "online" || current.connectionState === "offline") {
      return true;
    }
    if (fetched.connectionState === "online" || fetched.connectionState === "offline") {
      return false;
    }
  }

  return current.connected === true && fetched.connected !== true;
};

const mergeRealtimeSnapshots = (
  currentList: Device.Realtime[],
  fetchedList: Device.Realtime[]
) => {
  if (currentList.length === 0) return fetchedList;
  if (fetchedList.length === 0) return currentList;

  const currentMap = new Map(currentList.map((item) => [item.id, item]));
  const fetchedMap = new Map(fetchedList.map((item) => [item.id, item]));
  const mergedIds = new Set<number>([...currentMap.keys(), ...fetchedMap.keys()]);

  const mergedList: Device.Realtime[] = [];
  for (const id of mergedIds) {
    const current = currentMap.get(id);
    const fetched = fetchedMap.get(id);

    if (!current) {
      if (fetched) mergedList.push(fetched);
      continue;
    }

    if (!fetched || shouldPreferCurrentRealtime(current, fetched)) {
      mergedList.push(current);
      continue;
    }

    mergedList.push({
      ...current,
      ...fetched,
      connectionState: fetched.connectionState ?? current.connectionState,
    });
  }

  return mergedList;
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
  const queryClient = useQueryClient();
  return useQuery({
    queryKey: deviceKeys.realtime(),
    queryFn: async () => {
      const fetched = await deviceApi.getRealtime();
      const current = queryClient.getQueryData<PaginatedResult<Device.Realtime>>(deviceKeys.realtime());

      if (!current?.list?.length) {
        return fetched;
      }

      return {
        ...fetched,
        list: mergeRealtimeSnapshots(current.list, fetched.list ?? []),
      };
    },
    staleTime: 0,
    refetchOnMount: "always",
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
        connectionState: realtime?.connectionState,
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
  const isLoading = staticQuery.isLoading && mergedList.length === 0;
  const isFetching = staticQuery.isFetching || realtimeQuery.isFetching;

  return useMemo(
    () => ({
      data,
      isLoading,
      isFetching,
      isError: staticQuery.isError || realtimeQuery.isError,
      error: staticQuery.error || realtimeQuery.error,
      refetch,
    }),
    [
      data,
      realtimeQuery.isFetching,
      isFetching,
      isLoading,
      realtimeQuery.error,
      realtimeQuery.isError,
      refetch,
      staticQuery.error,
      staticQuery.isError,
      staticQuery.isLoading,
      staticQuery.isFetching,
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

/** 设备分享列表 Query */
export function useDeviceShares(
  deviceId: number,
  options?: Omit<UseQueryOptions<Device.ShareItem[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: deviceKeys.shares(deviceId),
    queryFn: () => deviceApi.getShares(deviceId),
    enabled: deviceId > 0,
    ...options,
  });
}
