/**
 * 开放接入 API
 */

import type { OpenAccess, PageResult } from "@/types";
import { request } from "../common";

const ENDPOINTS = {
  ACCESS_KEYS: "/api/open-access-key",
  ACCESS_KEY: (id: number) => `/api/open-access-key/${id}`,
  ROTATE_ACCESS_KEY: (id: number) => `/api/open-access-key/${id}/rotate`,
  WEBHOOKS: "/api/open-webhook",
  WEBHOOK: (id: number) => `/api/open-webhook/${id}`,
  ACCESS_LOGS: "/api/open-access-log",
} as const;

export function getAccessKeys() {
  return request.get<OpenAccess.AccessKeyItem[]>(ENDPOINTS.ACCESS_KEYS);
}

export function createAccessKey(data: OpenAccess.AccessKeyPayload) {
  return request.post<OpenAccess.AccessKeyCreateResult>(ENDPOINTS.ACCESS_KEYS, data);
}

export function updateAccessKey(id: number, data: OpenAccess.AccessKeyPayload) {
  return request.put<void>(ENDPOINTS.ACCESS_KEY(id), data);
}

export function rotateAccessKey(id: number) {
  return request.post<OpenAccess.AccessKeyCreateResult>(ENDPOINTS.ROTATE_ACCESS_KEY(id));
}

export function removeAccessKey(id: number) {
  return request.delete<void>(ENDPOINTS.ACCESS_KEY(id));
}

export function getWebhooks(params?: OpenAccess.WebhookQuery) {
  return request.get<OpenAccess.WebhookItem[]>(ENDPOINTS.WEBHOOKS, { params });
}

export function createWebhook(data: OpenAccess.WebhookPayload) {
  return request.post<void>(ENDPOINTS.WEBHOOKS, data);
}

export function updateWebhook(id: number, data: Partial<OpenAccess.WebhookPayload>) {
  return request.put<void>(ENDPOINTS.WEBHOOK(id), data);
}

export function removeWebhook(id: number) {
  return request.delete<void>(ENDPOINTS.WEBHOOK(id));
}

export function getAccessLogs(params?: OpenAccess.AccessLogQuery) {
  return request.get<PageResult<OpenAccess.AccessLogItem>>(ENDPOINTS.ACCESS_LOGS, { params });
}
