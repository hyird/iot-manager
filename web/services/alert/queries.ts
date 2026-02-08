/**
 * 告警管理 Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { Alert } from "@/types";
import type { PaginatedResult } from "../common";
import * as alertApi from "./api";
import { alertKeys } from "./keys";

/** 告警规则列表 Query */
export function useAlertRuleList(
  params?: {
    page?: number;
    pageSize?: number;
    keyword?: string;
    deviceId?: number;
    severity?: string;
  },
  options?: Omit<UseQueryOptions<PaginatedResult<Alert.RuleItem>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: alertKeys.ruleList(params),
    queryFn: () => alertApi.getRules(params),
    ...options,
  });
}

/** 告警记录列表 Query */
export function useAlertRecordList(
  params?: {
    page?: number;
    pageSize?: number;
    deviceId?: number;
    ruleId?: number;
    status?: string;
    severity?: string;
  },
  options?: Omit<UseQueryOptions<PaginatedResult<Alert.RecordItem>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: alertKeys.recordList(params),
    queryFn: () => alertApi.getRecords(params),
    ...options,
  });
}

/** 活跃告警统计 Query */
export function useAlertStats(
  options?: Omit<UseQueryOptions<Alert.ActiveStats>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: alertKeys.stats(),
    queryFn: () => alertApi.getActiveStats(),
    ...options,
  });
}
