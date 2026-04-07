/**
 * 链路管理 Mutation Hooks
 */

import type { Link } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import { deviceKeys } from "../device/keys";
import * as api from "./api";
import { linkQueryKeys } from "./keys";

/** 保存链路（创建或更新） */
export function useLinkSave() {
  return useSaveMutationWithFeedback<
    Link.CreateDto & { id?: number },
    Link.CreateDto,
    Link.UpdateDto
  >({
    createFn: api.create,
    updateFn: api.update,
    toUpdatePayload: ({ id: _id, ...data }) => data,
    createMessage: "保存成功",
    updateMessage: "保存成功",
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
