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
import { getAntdMessage } from "@/providers/AntdMessageHolder";
import { store } from "@/store";
import { clearAuth, refreshAccessToken } from "@/store/slices/authSlice";
import { redirectToLogin } from "@/utils/navigation";
import {
  ApiError,
  getApiResponseCode,
  getApiResponseMessage,
  isApiResponseEnvelope,
} from "./apiError";

/** 请求配置 - 额外支持静默和重试标记 */
export interface RequestConfig extends AxiosRequestConfig {
  _retry?: boolean;
  _silent?: boolean;
}

/** 经过响应拦截器处理后的请求接口 - 直接返回数据而非 AxiosResponse */
interface RequestInstance extends AxiosInstance {
  get<T = unknown>(url: string, config?: RequestConfig): Promise<T>;
  post<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
  put<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
  delete<T = unknown>(url: string, config?: RequestConfig): Promise<T>;
  patch<T = unknown>(url: string, data?: unknown, config?: RequestConfig): Promise<T>;
}

const request = axios.create({
  baseURL: "/",
  timeout: 30000,
}) as RequestInstance;

function buildApiError(
  message: string,
  options: {
    code?: number;
    status?: number;
    data?: unknown;
    source?: ApiError["source"];
  } = {}
) {
  return new ApiError(message, options);
}

function buildApiErrorFromResponse(
  response: { data: unknown; status: number },
  fallbackMessage = "请求失败"
) {
  const payload = response.data;
  const message = getApiResponseMessage(payload, fallbackMessage);
  const code = getApiResponseCode(payload);

  return buildApiError(message, {
    code,
    status: response.status,
    data: payload,
    source: "response",
  });
}

function buildApiErrorFromAxiosError(error: AxiosError<unknown>) {
  if (!error.response) {
    const isTimeout = error.code === "ECONNABORTED";
    return buildApiError(isTimeout ? "请求超时" : "网络连接失败", {
      status: isTimeout ? 408 : 0,
      data: error,
      source: isTimeout ? "timeout" : "network",
    });
  }

  return buildApiErrorFromResponse({
    data: error.response.data,
    status: error.response.status,
  }, error.message || "请求失败");
}

// 请求拦截器
request.interceptors.request.use(
  (config: InternalAxiosRequestConfig) => {
    const state = store.getState();
    const token = state.auth.token;
    if (token && config.headers) {
      const headers = config.headers as Record<string, unknown> & {
        has?: (name: string) => boolean;
      };
      const hasAuthorization =
        typeof headers.has === "function"
          ? headers.has("Authorization") || headers.has("authorization")
          : "Authorization" in headers || "authorization" in headers;

      if (!hasAuthorization) {
        headers.Authorization = `Bearer ${token}`;
      }
    }
    return config;
  },
  (error) => Promise.reject(error)
);

// Token 刷新状态
let isRefreshing = false;
let refreshSubscribers: Array<{
  onSuccess: (token: string) => void;
  onError: (error: ApiError) => void;
}> = [];

function subscribeTokenRefresh(onSuccess: (token: string) => void, onError: (error: ApiError) => void) {
  refreshSubscribers.push({ onSuccess, onError });
}

function onTokenRefreshed(token: string) {
  refreshSubscribers.forEach(({ onSuccess }) => {
    onSuccess(token);
  });
  refreshSubscribers = [];
}

function onTokenRefreshFailed(error: ApiError) {
  refreshSubscribers.forEach(({ onError }) => {
    onError(error);
  });
  refreshSubscribers = [];
}

function handleAuthExpired(error: ApiError) {
  isRefreshing = false;
  onTokenRefreshFailed(error);
  store.dispatch(clearAuth());
  redirectToLogin();
  return Promise.reject(error);
}

// 响应拦截器
request.interceptors.response.use(
  (response): any => {
    const data = response.data as unknown;
    const isSilent = (response.config as RequestConfig | undefined)?._silent;

    if (isApiResponseEnvelope(data) && data.code !== 0) {
      const apiError = buildApiErrorFromResponse({
        data,
        status: response.status,
      });
      if (!isSilent) {
        getAntdMessage()?.error(apiError.message);
      }
      return Promise.reject(apiError);
    }

    return isApiResponseEnvelope(data) && data.data !== undefined ? data.data : data;
  },
  async (error: AxiosError<unknown>): Promise<any> => {
    const originalRequest = error.config as RequestConfig | undefined;
    const requestUrl = originalRequest?.url || "";
    const isSilent = originalRequest?._silent ?? false;
    const isAuthRefreshRequest = requestUrl.includes("/api/auth/refresh");

    // 静默模式：不弹任何错误提示，也不触发全局刷新逻辑
    if (isSilent) {
      return Promise.reject(buildApiErrorFromAxiosError(error));
    }

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
            (refreshError: ApiError) => {
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

        const refreshError = buildApiError("登录状态已失效，请重新登录", {
          status: 401,
          source: "auth-refresh",
        });
        return handleAuthExpired(refreshError);
      } catch {
        const refreshError = buildApiError("登录状态已失效，请重新登录", {
          status: 401,
          source: "auth-refresh",
        });
        return handleAuthExpired(refreshError);
      } finally {
        isRefreshing = false;
      }
    }

    const apiError = buildApiErrorFromAxiosError(error);

    // 无响应：网络错误 / 超时
    if (!error.response) {
      if (error.code === "ECONNABORTED") {
        notification.error({ message: "请求超时", description: "网络连接较慢，请稍后重试" });
      } else {
        notification.error({ message: "网络连接失败", description: "请检查网络连接是否正常" });
      }
      return Promise.reject(apiError);
    }

    // 服务器错误 (5xx) - 统一使用 notification，提前返回避免重复提示
    if (error.response.status >= 500) {
      notification.error({ message: "服务器错误", description: "服务器遇到问题，请稍后重试" });
      return Promise.reject(
        buildApiError(apiError.message || "服务器错误", {
          code: apiError.code,
          status: apiError.status ?? error.response.status,
          data: apiError.data,
          source: "server",
        })
      );
    }

    getAntdMessage()?.error(apiError.message || error.message || "请求失败");
    return Promise.reject(apiError);
  }
);

/** 重置 Token 刷新状态（登出时调用，防止残留状态影响新会话） */
export function resetRefreshState() {
  isRefreshing = false;
  refreshSubscribers = [];
}

export default request;
