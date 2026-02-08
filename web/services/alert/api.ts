/**
 * 告警管理 API
 */

import type { Alert } from "@/types";
import { type PaginatedResult, request } from "../common";

/** API 端点 */
const ENDPOINTS = {
  RULE: "/api/alert/rule",
  RULE_DETAIL: (id: number) => `/api/alert/rule/${id}`,
  RULE_BATCH: "/api/alert/rule/batch",
  RULE_APPLY_TEMPLATE: "/api/alert/rule/apply-template",
  TEMPLATE: "/api/alert/template",
  TEMPLATE_DETAIL: (id: number) => `/api/alert/template/${id}`,
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

/** 批量删除规则 */
export function batchDeleteRules(ids: number[]) {
  return request.delete<void>(ENDPOINTS.RULE_BATCH, { data: { ids } });
}

/** 获取模板列表 */
export function getTemplates(params?: { page?: number; pageSize?: number; category?: string }) {
  return request.get<PaginatedResult<Alert.TemplateItem>>(ENDPOINTS.TEMPLATE, { params });
}

/** 获取模板详情 */
export function getTemplateDetail(id: number) {
  return request.get<Alert.TemplateDetail>(ENDPOINTS.TEMPLATE_DETAIL(id));
}

/** 创建模板 */
export function createTemplate(data: Alert.TemplateDto) {
  return request.post<void>(ENDPOINTS.TEMPLATE, data);
}

/** 更新模板 */
export function updateTemplate(id: number, data: Partial<Alert.TemplateDto>) {
  return request.put<void>(ENDPOINTS.TEMPLATE_DETAIL(id), data);
}

/** 删除模板 */
export function deleteTemplate(id: number) {
  return request.delete<void>(ENDPOINTS.TEMPLATE_DETAIL(id));
}

/** 应用模板到多个设备 */
export function applyTemplate(data: Alert.ApplyTemplateRequest) {
  return request.post<Alert.ApplyTemplateResponse>(ENDPOINTS.RULE_APPLY_TEMPLATE, data);
}
