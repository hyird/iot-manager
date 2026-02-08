import { useMemo } from "react";
import { Navigate, Outlet, useLocation } from "react-router-dom";
import { useAuthStore } from "@/store/hooks";

export function AuthGuard() {
  const { token } = useAuthStore();
  const location = useLocation();

  // 使用 useMemo 缓存 state 对象，避免每次渲染都创建新引用
  const redirectState = useMemo(
    () => ({ from: { pathname: location.pathname } }),
    [location.pathname]
  );

  if (!token) {
    return <Navigate to="/login" state={redirectState} replace />;
  }

  return <Outlet />;
}
