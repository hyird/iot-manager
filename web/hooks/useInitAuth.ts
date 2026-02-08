import { useCurrentUser } from "@/services";
import { useAuthStore } from "@/store/hooks";

/**
 * 初始化用户认证数据
 * 使用 TanStack Query 管理用户数据获取和刷新
 */
export function useInitAuth() {
  const { token } = useAuthStore();
  const { data: user } = useCurrentUser();

  return { token, user };
}
