import { useEffect, useRef } from "react";
import { useAuthStore, usePermissionStore } from "@/store/hooks";
import { store } from "@/store";
import { clearAuth } from "@/store/slices/authSlice";

/**
 * 初始化用户认证数据
 * 处理 token 存在但用户信息未加载的情况
 */
export function useInitAuth() {
  const { token, user, refreshUser } = useAuthStore();
  const { setPermissions } = usePermissionStore();

  const refreshingRef = useRef(false);

  // 刷新用户信息
  useEffect(() => {
    if (token && !user && !refreshingRef.current) {
      refreshingRef.current = true;
      refreshUser()
        .unwrap()
        .catch(() => {
          store.dispatch(clearAuth());
        })
        .finally(() => {
          refreshingRef.current = false;
        });
    }
  }, [token, user, refreshUser]);

  // 同步权限数据
  useEffect(() => {
    if (user?.menus && user?.roles) {
      setPermissions(user.menus, user.roles);
    }
  }, [user, setPermissions]);

  return { token, user };
}
