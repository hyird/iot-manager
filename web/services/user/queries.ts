/**
 * 用户管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { User } from "@/types";
import type { PaginatedResult } from "../common";
import * as api from "./api";
import { userQueryKeys } from "./keys";

type UserListResult = PaginatedResult<User.Item>;

/**
 * 获取用户列表
 */
export function useUserList(
  params?: User.Query,
  options?: Omit<UseQueryOptions<UserListResult>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: userQueryKeys.list(params),
    queryFn: () => api.getList(params),
    ...options,
  });
}

/**
 * 获取用户详情
 */
export function useUserDetail(
  id: number,
  options?: Omit<UseQueryOptions<User.Item>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: userQueryKeys.detail(id),
    queryFn: () => api.getDetail(id),
    enabled: !!id,
    ...options,
  });
}
