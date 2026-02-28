/**
 * 链路管理 Mutation Hooks
 */

import type { Link } from "@/types";
import { useMutationWithFeedback } from "../common";
import { deviceKeys } from "../device/keys";
import * as api from "./api";
import { linkQueryKeys } from "./keys";

/** 创建链路 */
export function useLinkCreate() {
  return useMutationWithFeedback({
    mutationFn: api.create,
    successMessage: "创建成功",
    invalidateKeys: [linkQueryKeys.all, deviceKeys.all],
  });
}

/** 更新链路 */
export function useLinkUpdate() {
  return useMutationWithFeedback({
    mutationFn: ({ id, data }: { id: number; data: Link.UpdateDto }) => api.update(id, data),
    successMessage: "更新成功",
    invalidateKeys: [linkQueryKeys.all, deviceKeys.all],
  });
}

/** 删除链路 */
export function useLinkDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    invalidateKeys: [linkQueryKeys.all, deviceKeys.all],
  });
}

/** 保存链路（创建或更新） */
export function useLinkSave() {
  return useMutationWithFeedback({
    mutationFn: async (values: Link.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    successMessage: "保存成功",
    invalidateKeys: [linkQueryKeys.all, deviceKeys.all],
  });
}
