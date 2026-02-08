/**
 * 角色管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { Role } from "@/types";
import type { PaginatedResult } from "../common";
import * as api from "./api";
import { roleQueryKeys } from "./keys";

type RoleListResult = PaginatedResult<Role.Item>;

/**
 * 获取角色列表
 */
export function useRoleList(
  params?: Role.Query,
  options?: Omit<UseQueryOptions<RoleListResult>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: roleQueryKeys.list(params),
    queryFn: () => api.getList(params),
    ...options,
  });
}

/**
 * 获取角色详情
 */
export function useRoleDetail(
  id: number,
  options?: Omit<UseQueryOptions<Role.Detail>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: roleQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: id > 0,
    ...options,
  });
}

/**
 * 获取所有启用角色（用于下拉选择）
 */
export function useRoleOptions(
  options?: Omit<UseQueryOptions<PaginatedResult<Role.Option>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: roleQueryKeys.options(),
    queryFn: api.getAll,
    staleTime: 5 * 60 * 1000, // 5分钟缓存
    ...options,
  });
}
