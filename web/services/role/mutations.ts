/**
 * 角色管理 Mutation Hooks
 */

import type { Role } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { roleQueryKeys } from "./keys";

/** 创建角色 */
export function useRoleCreate() {
  return useMutationWithFeedback({
    mutationFn: api.create,
    successMessage: "创建成功",
    invalidateKeys: [roleQueryKeys.all],
  });
}

/** 更新角色 */
export function useRoleUpdate() {
  return useMutationWithFeedback({
    mutationFn: ({ id, data }: { id: number; data: Role.UpdateDto }) => api.update(id, data),
    successMessage: "更新成功",
    invalidateKeys: [roleQueryKeys.all],
  });
}

/** 删除角色 */
export function useRoleDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [roleQueryKeys.all],
  });
}

/** 保存角色（创建或更新） */
export function useRoleSave() {
  return useMutationWithFeedback({
    mutationFn: async (values: Role.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    successMessage: "保存成功",
    invalidateKeys: [roleQueryKeys.all],
  });
}
