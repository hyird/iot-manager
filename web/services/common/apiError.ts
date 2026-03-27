/**
 * 前端统一 API 错误抽象
 */

export type ApiErrorSource = "response" | "network" | "timeout" | "server" | "auth-refresh";

export interface ApiResponseEnvelope<T = unknown> {
  code: number;
  message: string;
  data?: T;
  status?: number;
}

export interface ApiErrorOptions {
  code?: number;
  status?: number;
  data?: unknown;
  source?: ApiErrorSource;
}

export class ApiError extends Error {
  code?: number;
  status?: number;
  data?: unknown;
  source: ApiErrorSource;

  constructor(message: string, options: ApiErrorOptions = {}) {
    super(message);
    this.name = "ApiError";
    this.code = options.code;
    this.status = options.status;
    this.data = options.data;
    this.source = options.source ?? "response";

    Object.setPrototypeOf(this, ApiError.prototype);
  }
}

export function isApiResponseEnvelope(value: unknown): value is ApiResponseEnvelope {
  if (typeof value !== "object" || value === null) return false;

  const record = value as Record<string, unknown>;
  return typeof record.code === "number" && typeof record.message === "string";
}

export function getApiResponseMessage(value: unknown, fallback = "请求失败"): string {
  if (!isApiResponseEnvelope(value)) return fallback;

  const message = value.message.trim();
  return message || fallback;
}

export function getApiResponseCode(value: unknown): number | undefined {
  return isApiResponseEnvelope(value) ? value.code : undefined;
}
