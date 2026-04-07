/**
 * 告警管理 Mutation Hooks
 */

import type { Alert } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as alertApi from "./api";
import { alertKeys } from "./keys";

/** 保存告警规则 Mutation (创建或更新) */
export function useAlertRuleSave() {
  return useSaveMutationWithFeedback<
    Alert.RuleDto & { id?: number },
    Alert.RuleDto,
    Alert.RuleDto
  >({
    createFn: alertApi.createRule,
    updateFn: (id, data) => alertApi.updateRule(id, data),
    toUpdatePayload: ({ id: _id, ...rest }) => rest,
    invalidateKeys: [alertKeys.all],
  });
}

/** 删除告警规则 Mutation */
export function useAlertRuleDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => alertApi.deleteRule(id),
    successMessage: "删除成功",
    invalidateKeys: [alertKeys.all],
  });
}

/** 批量删除告警规则 Mutation */
export function useAlertRuleBatchDelete() {
  return useMutationWithFeedback({
    mutationFn: (ids: number[]) => alertApi.batchDeleteRules(ids),
    successMessage: "批量删除成功",
    invalidateKeys: [alertKeys.all],
  });
}

/** 确认告警 Mutation */
export function useAlertAcknowledge() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => alertApi.acknowledgeRecord(id),
    successMessage: "确认成功",
    invalidateKeys: [alertKeys.all],
  });
}

/** 批量确认告警 Mutation */
export function useAlertBatchAcknowledge() {
  return useMutationWithFeedback({
    mutationFn: (ids: number[]) => alertApi.batchAcknowledge(ids),
    successMessage: "批量确认成功",
    invalidateKeys: [alertKeys.all],
  });
}

/** 保存告警模板 Mutation (创建或更新) */
export function useAlertTemplateSave() {
  return useSaveMutationWithFeedback<
    Alert.TemplateDto & { id?: number },
    Alert.TemplateDto,
    Alert.TemplateDto
  >({
    createFn: alertApi.createTemplate,
    updateFn: (id, data) => alertApi.updateTemplate(id, data),
    toUpdatePayload: ({ id: _id, ...rest }) => rest,
    invalidateKeys: [alertKeys.all],
  });
}

/** 删除告警模板 Mutation */
export function useAlertTemplateDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => alertApi.deleteTemplate(id),
    successMessage: "删除成功",
    invalidateKeys: [alertKeys.all],
  });
}

/** 应用模板到设备 Mutation */
export function useAlertApplyTemplate() {
  return useMutationWithFeedback({
    mutationFn: (data: Alert.ApplyTemplateRequest) => alertApi.applyTemplate(data),
    successMessage: (result) =>
      `应用成功，已创建 ${(result as Alert.ApplyTemplateResponse).success} 条规则`,
    invalidateKeys: [alertKeys.all],
  });
}
