/**
 * 告警管理 API
 */

import type { Alert } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  RULE: "/api/alert/rule",
  RULE_DETAIL: (id: number) => `/api/alert/rule/${id}`,
  RECORD: "/api/alert/record",
  RECORD_ACK: (id: number) => `/api/alert/record/${id}/ack`,
  BATCH_ACK: "/api/alert/record/batch-ack",
  STATS: "/api/alert/stats",
} as const;

/** 获取规则列表 */
export function getRules(params?: {
  page?: number;
  pageSize?: number;
  keyword?: string;
  deviceId?: number;
  severity?: string;
}) {
  return request.get<PaginatedResult<Alert.RuleItem>>(ENDPOINTS.RULE, { params });
}

/** 获取规则详情 */
export function getRuleDetail(id: number) {
  return request.get<Alert.RuleItem>(ENDPOINTS.RULE_DETAIL(id));
}

/** 创建规则 */
export function createRule(data: Alert.RuleDto) {
  return request.post<void>(ENDPOINTS.RULE, data);
}

/** 更新规则 */
export function updateRule(id: number, data: Alert.RuleDto) {
  return request.put<void>(ENDPOINTS.RULE_DETAIL(id), data);
}

/** 删除规则 */
export function deleteRule(id: number) {
  return request.delete<void>(ENDPOINTS.RULE_DETAIL(id));
}

/** 获取告警记录列表 */
export function getRecords(params?: {
  page?: number;
  pageSize?: number;
  deviceId?: number;
  ruleId?: number;
  status?: string;
  severity?: string;
}) {
  return request.get<PaginatedResult<Alert.RecordItem>>(ENDPOINTS.RECORD, { params });
}

/** 确认告警 */
export function acknowledgeRecord(id: number) {
  return request.post<void>(ENDPOINTS.RECORD_ACK(id));
}

/** 批量确认告警 */
export function batchAcknowledge(ids: number[]) {
  return request.post<void>(ENDPOINTS.BATCH_ACK, { ids });
}

/** 获取活跃告警统计 */
export function getActiveStats() {
  return request.get<Alert.ActiveStats>(ENDPOINTS.STATS);
}
