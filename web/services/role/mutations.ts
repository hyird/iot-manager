/**
 * 角色管理 Mutation Hooks
 */

import type { Role } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as api from "./api";
import { roleQueryKeys } from "./keys";

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
    invalidateKeys: [roleQueryKeys.all],
  });
}

/** 保存角色（创建或更新） */
export function useRoleSave() {
  return useSaveMutationWithFeedback<
    Role.CreateDto & { id?: number },
    Role.CreateDto,
    Role.UpdateDto
  >({
    createFn: api.create,
    updateFn: api.update,
    toUpdatePayload: ({ id: _id, ...data }) => data,
    createMessage: "保存成功",
    updateMessage: "保存成功",
    invalidateKeys: [roleQueryKeys.all],
  });
}
