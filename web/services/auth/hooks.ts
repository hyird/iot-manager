/**
 * 认证相关 Hooks
 * 注意：认证状态由 Redux 管理，这里只提供便捷的 hooks
 */

import { useMutation } from "@tanstack/react-query";
import { App } from "antd";
import { useNavigate } from "react-router-dom";
import { useAppDispatch, useAppSelector } from "@/store/hooks";
import { clearAuth, selectRefreshToken, setAuth } from "@/store/slices/authSlice";
import type { Auth } from "@/types";
import * as api from "./api";

/**
 * 登录 Hook
 */
export function useLogin() {
  const dispatch = useAppDispatch();
  const navigate = useNavigate();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.login,
    onSuccess: (data: Auth.LoginResult) => {
      dispatch(
        setAuth({
          token: data.token,
          refreshToken: data.refreshToken,
          user: data.user,
        })
      );
      message.success("登录成功");
      navigate("/", { replace: true });
    },
  });
}

/**
 * 登出 Hook
 */
export function useLogout() {
  const dispatch = useAppDispatch();
  const navigate = useNavigate();
  const refreshToken = useAppSelector(selectRefreshToken);

  return useMutation({
    mutationFn: () => api.logout(refreshToken ?? undefined),
    onSettled: () => {
      // 无论成功失败都清除本地状态
      dispatch(clearAuth());
      navigate("/login", { replace: true });
    },
  });
}
