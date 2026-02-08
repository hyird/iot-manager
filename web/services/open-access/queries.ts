/**
 * 开放接入 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { OpenAccess, PageResult } from "@/types";
import * as api from "./api";
import { openAccessKeys } from "./keys";

export function useOpenAccessKeys(
  options?: Omit<UseQueryOptions<OpenAccess.AccessKeyItem[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: openAccessKeys.accessKeys(),
    queryFn: api.getAccessKeys,
    ...options,
  });
}

export function useOpenWebhooks(
  params?: OpenAccess.WebhookQuery,
  options?: Omit<UseQueryOptions<OpenAccess.WebhookItem[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: openAccessKeys.webhooks(params),
    queryFn: () => api.getWebhooks(params),
    ...options,
  });
}

export function useOpenAccessLogs(
  params?: OpenAccess.AccessLogQuery,
  options?: Omit<UseQueryOptions<PageResult<OpenAccess.AccessLogItem>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: openAccessKeys.logs(params),
    queryFn: () => api.getAccessLogs(params),
    ...options,
  });
}
