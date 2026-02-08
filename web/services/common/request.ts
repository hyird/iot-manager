/**
 * HTTP 请求基础配置
 * 从原 api/request.ts 迁移，作为 Service 层的底层依赖
 */

import { notification } from "antd";
import axios, {
  type AxiosError,
  type AxiosInstance,
  type AxiosRequestConfig,
  type InternalAxiosRequestConfig,
} from "axios";
import { store } from "@/store";
import { clearAuth, refreshAccessToken } from "@/store/slices/authSlice";

/** 经过响应拦截器处理后的请求接口 - 直接返回数据而非 AxiosResponse */
interface RequestInstance extends AxiosInstance {
  get<T = unknown>(url: string, config?: AxiosRequestConfig): Promise<T>;
  post<T = unknown>(url: string, data?: unknown, config?: AxiosRequestConfig): Promise<T>;
  put<T = unknown>(url: string, data?: unknown, config?: AxiosRequestConfig): Promise<T>;
  delete<T = unknown>(url: string, config?: AxiosRequestConfig): Promise<T>;
  patch<T = unknown>(url: string, data?: unknown, config?: AxiosRequestConfig): Promise<T>;
}

const request = axios.create({
  baseURL: "/",
  timeout: 30000,
}) as RequestInstance;

// 请求拦截器
request.interceptors.request.use(
  (config: InternalAxiosRequestConfig) => {
    const state = store.getState();
    const token = state.auth.token;
    if (token && config.headers) {
      config.headers.Authorization = `Bearer ${token}`;
    }
    return config;
  },
  (error) => Promise.reject(error)
);

// Token 刷新状态
let isRefreshing = false;
let refreshSubscribers: Array<{
  onSuccess: (token: string) => void;
  onError: (error: Error) => void;
}> = [];

function subscribeTokenRefresh(onSuccess: (token: string) => void, onError: (error: Error) => void) {
  refreshSubscribers.push({ onSuccess, onError });
}

function onTokenRefreshed(token: string) {
  refreshSubscribers.forEach(({ onSuccess }) => {
    onSuccess(token);
  });
  refreshSubscribers = [];
}

function onTokenRefreshFailed(error: Error) {
  refreshSubscribers.forEach(({ onError }) => {
    onError(error);
  });
  refreshSubscribers = [];
}

// 响应拦截器
request.interceptors.response.use(
  (response) => {
    const data = response.data;
    if (data.code !== undefined && data.code !== 0) {
      return Promise.reject(new Error(data.message || "请求失败"));
    }
    return data.data !== undefined ? data.data : data;
  },
  async (error: AxiosError<{ message?: string }>) => {
    const originalRequest = error.config as InternalAxiosRequestConfig & { _retry?: boolean };
    const requestUrl = originalRequest?.url || "";
    const isAuthRefreshRequest = requestUrl.includes("/api/auth/refresh");

    // 401 处理：尝试刷新 token
    if (
      error.response?.status === 401 &&
      originalRequest &&
      !originalRequest._retry &&
      !isAuthRefreshRequest
    ) {
      if (isRefreshing) {
        return new Promise((resolve, reject) => {
          subscribeTokenRefresh(
            (token: string) => {
              if (originalRequest.headers) {
                originalRequest.headers.Authorization = `Bearer ${token}`;
              }
              resolve(request(originalRequest));
            },
            (refreshError: Error) => {
              reject(refreshError);
            }
          );
        });
      }

      originalRequest._retry = true;
      isRefreshing = true;

      try {
        const resultAction = await store.dispatch(refreshAccessToken());
        if (refreshAccessToken.fulfilled.match(resultAction)) {
          const newToken = resultAction.payload.token;
          onTokenRefreshed(newToken);
          if (originalRequest.headers) {
            originalRequest.headers.Authorization = `Bearer ${newToken}`;
          }
          return request(originalRequest);
        }

        const refreshError = new Error("登录状态已失效，请重新登录");
        onTokenRefreshFailed(refreshError);
        store.dispatch(clearAuth());
        window.location.href = "/login";
        return Promise.reject(refreshError);
      } catch {
        const refreshError = new Error("登录状态已失效，请重新登录");
        onTokenRefreshFailed(refreshError);
        store.dispatch(clearAuth());
        window.location.href = "/login";
        return Promise.reject(refreshError);
      } finally {
        isRefreshing = false;
      }
    }

    // 无响应：网络错误 / 超时
    if (!error.response) {
      if (error.code === "ECONNABORTED") {
        notification.error({ message: "请求超时", description: "网络连接较慢，请稍后重试" });
      } else {
        notification.error({ message: "网络连接失败", description: "请检查网络连接是否正常" });
      }
      return Promise.reject(error);
    }

    // 服务器错误 (5xx)
    if (error.response.status >= 500) {
      notification.error({ message: "服务器错误", description: "服务器遇到问题，请稍后重试" });
    }

    const message = error.response?.data?.message || error.message || "请求失败";
    return Promise.reject(new Error(message));
  }
);

export default request;
