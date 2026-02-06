/**
 * 协议配置 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { Protocol } from "@/types";
import type { PaginatedResult } from "../common";
import * as api from "./api";
import { protocolQueryKeys } from "./keys";

/** 获取协议配置列表 */
export function useProtocolConfigList(
  params?: Protocol.Query,
  options?: Omit<UseQueryOptions<PaginatedResult<Protocol.Item>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: protocolQueryKeys.list(params),
    queryFn: () => api.getList(params),
    ...options,
  });
}

/** 获取协议配置详情 */
export function useProtocolConfigDetail(
  id: number,
  options?: Omit<UseQueryOptions<Protocol.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: protocolQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: !!id,
    ...options,
  });
}

/** 获取指定协议的配置选项 */
export function useProtocolConfigOptions(
  protocol: Protocol.Type,
  options?: Omit<UseQueryOptions<{ list: Protocol.Option[] }>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: protocolQueryKeys.options(protocol),
    queryFn: () => api.getOptions(protocol),
    enabled: !!protocol,
    staleTime: 60 * 1000, // 1分钟
    ...options,
  });
}
