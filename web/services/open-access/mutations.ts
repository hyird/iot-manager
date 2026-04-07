/**
 * 开放接入 Mutation Hooks
 */

import type { OpenAccess } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as api from "./api";
import { openAccessKeys } from "./keys";

export function useAccessKeySave() {
  return useMutationWithFeedback<OpenAccess.AccessKeyCreateResult | void, OpenAccess.AccessKeyPayload & {
    id?: number;
  }>({
    mutationFn: async (data) => {
      if (data.id != null) {
        const { id, ...rest } = data;
        await api.updateAccessKey(id, rest);
        return;
      }

      return api.createAccessKey(data);
    },
    successMessage: (_, variables) => (variables.id != null ? "更新成功" : "创建成功"),
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
  return useSaveMutationWithFeedback<
    (OpenAccess.WebhookPayload | Partial<OpenAccess.WebhookPayload>) & { id?: number }
    ,
    OpenAccess.WebhookPayload,
    Partial<OpenAccess.WebhookPayload>
  >({
    createFn: (data) => api.createWebhook(data as OpenAccess.WebhookPayload),
    updateFn: (id, data) => api.updateWebhook(id, data as Partial<OpenAccess.WebhookPayload>),
    toCreatePayload: (data) => data as OpenAccess.WebhookPayload,
    toUpdatePayload: ({ id: _id, ...rest }) => rest,
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
