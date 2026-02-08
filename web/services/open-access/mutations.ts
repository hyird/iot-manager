/**
 * 开放接入 Mutation Hooks
 */

import type { OpenAccess } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { openAccessKeys } from "./keys";

export function useAccessKeySave() {
  return useMutationWithFeedback<
    OpenAccess.AccessKeyCreateResult | undefined,
    OpenAccess.AccessKeyPayload & { id?: number }
  >({
    mutationFn: async (data) => {
      if (data.id) {
        const { id, ...rest } = data;
        await api.updateAccessKey(id, rest);
        return;
      }

      return api.createAccessKey(data);
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
    invalidateKeys: [openAccessKeys.all],
  });
}

export function useAccessKeyRotate() {
  return useMutationWithFeedback<OpenAccess.AccessKeyCreateResult, number>({
    mutationFn: (id) => api.rotateAccessKey(id),
    successMessage: "轮换成功",
    invalidateKeys: [openAccessKeys.all],
  });
}

export function useAccessKeyDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => api.removeAccessKey(id),
    successMessage: "删除成功",
    invalidateKeys: [openAccessKeys.all],
  });
}

export function useWebhookSave() {
  return useMutationWithFeedback<
    void,
    (OpenAccess.WebhookPayload | Partial<OpenAccess.WebhookPayload>) & {
      id?: number;
    }
  >({
    mutationFn: async (data) => {
      if (data.id) {
        const { id, ...rest } = data;
        await api.updateWebhook(id, rest);
        return;
      }

      await api.createWebhook(data as OpenAccess.WebhookPayload);
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
    invalidateKeys: [openAccessKeys.all],
  });
}

export function useWebhookDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => api.removeWebhook(id),
    successMessage: "删除成功",
    invalidateKeys: [openAccessKeys.all],
  });
}
