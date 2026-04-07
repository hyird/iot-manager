/**
 * 用户管理 Mutation Hooks
 */

import type { User } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as api from "./api";
import { userQueryKeys } from "./keys";

/** 删除用户 */
export function useUserDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    invalidateKeys: [userQueryKeys.all],
  });
}

/** 保存用户（创建或更新） */
export function useUserSave() {
  return useSaveMutationWithFeedback<
    User.CreateDto & { id?: number },
    User.CreateDto,
    User.UpdateDto
  >({
    createFn: api.create,
    updateFn: api.update,
    toUpdatePayload: ({ id: _id, username: _username, ...data }) => data as User.UpdateDto,
    createMessage: "保存成功",
    updateMessage: "保存成功",
    invalidateKeys: [userQueryKeys.all],
  });
}
