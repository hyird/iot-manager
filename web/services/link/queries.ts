/**
 * 链路管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { Link } from "@/types";
import type { PaginatedResult } from "../common";
import * as api from "./api";
import { linkQueryKeys } from "./keys";

type LinkListResult = PaginatedResult<Link.Item>;

/**
 * 获取链路列表
 */
export function useLinkList(
  params?: Link.Query,
  options?: Omit<UseQueryOptions<LinkListResult>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: linkQueryKeys.list(params),
    queryFn: () => api.getList(params),
    refetchInterval: 10000,
    refetchIntervalInBackground: false,
    ...options,
  });
}

/**
 * 获取链路详情
 */
export function useLinkDetail(
  id: number,
  options?: Omit<UseQueryOptions<Link.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: linkQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: id > 0,
    ...options,
  });
}

/**
 * 获取链路选项列表
 */
export function useLinkOptions(
  options?: Omit<UseQueryOptions<PaginatedResult<Link.Option>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: linkQueryKeys.options(),
    queryFn: api.getOptions,
    staleTime: 5 * 60 * 1000, // 5分钟缓存
    ...options,
  });
}

/**
 * 获取链路枚举值（模式和协议列表）
 */
export function useLinkEnums(options?: Omit<UseQueryOptions<Link.Enums>, "queryKey" | "queryFn">) {
  return useQuery({
    queryKey: linkQueryKeys.enums(),
    queryFn: api.getEnums,
    staleTime: 30 * 60 * 1000, // 30分钟缓存
    ...options,
  });
}

/**
 * 获取服务器公网 IP
 */
export function usePublicIp(
  options?: Omit<UseQueryOptions<{ ip: string }>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: linkQueryKeys.publicIp(),
    queryFn: api.getPublicIp,
    staleTime: 5 * 60 * 1000, // 5分钟缓存
    ...options,
  });
}
