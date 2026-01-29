/**
 * HTTP 请求基础配置
 * 从原 api/request.ts 迁移，作为 Service 层的底层依赖
 */

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
let refreshSubscribers: ((token: string) => void)[] = [];

function subscribeTokenRefresh(callback: (token: string) => void) {
  refreshSubscribers.push(callback);
}

function onTokenRefreshed(token: string) {
  refreshSubscribers.forEach((callback) => callback(token));
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

    // 401 处理：尝试刷新 token
    if (error.response?.status === 401 && originalRequest && !originalRequest._retry) {
      if (isRefreshing) {
        return new Promise((resolve) => {
          subscribeTokenRefresh((token: string) => {
            if (originalRequest.headers) {
              originalRequest.headers.Authorization = `Bearer ${token}`;
            }
            resolve(request(originalRequest));
          });
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
        } else {
          store.dispatch(clearAuth());
          window.location.href = "/login";
          return Promise.reject(error);
        }
      } catch {
        store.dispatch(clearAuth());
        window.location.href = "/login";
        return Promise.reject(error);
      } finally {
        isRefreshing = false;
      }
    }

    const message = error.response?.data?.message || error.message || "网络错误";
    return Promise.reject(new Error(message));
  }
);

export default request;
