/**
 * 认证相关 Queries
 */

import { useQuery } from "@tanstack/react-query";
import { store } from "@/store";
import { useAppSelector } from "@/store/hooks";
import { selectToken, setUser } from "@/store/slices/authSlice";
import { deepEqual } from "@/utils/deepEqual";
import * as api from "./api";

export const authKeys = {
  currentUser: ["auth", "currentUser"] as const,
};

/**
 * 获取当前用户信息
 * - 使用 Redux 持久化数据作为 initialData（页面刷新时立即可用）
 * - 自动后台刷新 + 定时轮询（替代 useHeartbeat）
 * - 查询结果同步到 Redux（用于菜单持久化和路由渲染）
 */
export function useCurrentUser() {
  const token = useAppSelector(selectToken);

  return useQuery({
    queryKey: authKeys.currentUser,
    queryFn: async () => {
      const user = await api.fetchCurrentUser();
      // 同步到 Redux（菜单持久化 + 路由渲染需要）
      const currentUser = store.getState().auth.user;
      if (!deepEqual(currentUser, user)) {
        store.dispatch(setUser(user));
      }
      return user;
    },
    enabled: !!token,
    // 使用 Redux 持久化用户数据作为初始值（页面刷新时立即渲染菜单）
    initialData: () => store.getState().auth.user ?? undefined,
    initialDataUpdatedAt: 0, // 标记为旧数据，触发后台刷新
    staleTime: 2 * 60 * 1000, // 2 分钟
    refetchInterval: 5 * 60 * 1000, // 5 分钟（替代心跳）
    refetchOnWindowFocus: true,
  });
}
