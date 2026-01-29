/**
 * 认证相关 Hooks
 * 注意：认证状态由 Redux 管理，这里只提供便捷的 hooks
 */

import { useMutation } from "@tanstack/react-query";
import { App } from "antd";
import { useNavigate } from "react-router-dom";
import { useAppDispatch } from "@/store/hooks";
import { setAuth, clearAuth } from "@/store/slices/authSlice";
import * as api from "./api";
import type { Auth } from "@/types";

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
    onError: (error: Error) => {
      message.error(error.message || "登录失败");
    },
  });
}

/**
 * 登出 Hook
 */
export function useLogout() {
  const dispatch = useAppDispatch();
  const navigate = useNavigate();

  return useMutation({
    mutationFn: api.logout,
    onSettled: () => {
      // 无论成功失败都清除本地状态
      dispatch(clearAuth());
      navigate("/login", { replace: true });
    },
  });
}
