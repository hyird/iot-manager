/**
 * 告警管理 Mutation Hooks
 */

import type { Alert } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as alertApi from "./api";
import { alertKeys } from "./keys";

/** 保存告警规则 Mutation (创建或更新) */
export function useAlertRuleSave() {
  return useMutationWithFeedback({
    mutationFn: async (data: Alert.RuleDto & { id?: number }): Promise<void> => {
      if (data.id) {
        const { id, ...rest } = data;
        await alertApi.updateRule(id, rest);
      } else {
        await alertApi.createRule(data);
      }
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
    invalidateKeys: [alertKeys.all],
  });
}

/** 删除告警规则 Mutation */
export function useAlertRuleDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => alertApi.deleteRule(id),
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
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
