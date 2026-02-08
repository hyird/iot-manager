/**
 * 用户管理 Mutation Hooks
 */

import type { User } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { userQueryKeys } from "./keys";

/** 创建用户 */
export function useUserCreate() {
  return useMutationWithFeedback({
    mutationFn: api.create,
    successMessage: "创建成功",
    invalidateKeys: [userQueryKeys.all],
  });
}

/** 更新用户 */
export function useUserUpdate() {
  return useMutationWithFeedback({
    mutationFn: ({ id, data }: { id: number; data: User.UpdateDto }) => api.update(id, data),
    successMessage: "更新成功",
    invalidateKeys: [userQueryKeys.all],
  });
}

/** 删除用户 */
export function useUserDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [userQueryKeys.all],
  });
}

/** 保存用户（创建或更新） */
export function useUserSave() {
  return useMutationWithFeedback({
    mutationFn: async (values: User.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, username: _, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    successMessage: "保存成功",
    invalidateKeys: [userQueryKeys.all],
  });
}
