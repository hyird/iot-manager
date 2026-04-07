/**
 * 部门管理 Mutation Hooks
 */

import type { Department } from "@/types";
import { useSaveMutationWithFeedback } from "../common";
import * as api from "./api";
import { departmentQueryKeys } from "./keys";

/** 保存部门（创建或更新） */
export function useDepartmentSave() {
  return useSaveMutationWithFeedback<
    Department.CreateDto & { id?: number },
    Department.CreateDto,
    Department.UpdateDto
  >({
    createFn: api.create,
    updateFn: api.update,
    toUpdatePayload: ({ id: _id, ...data }) => data,
    createMessage: "保存成功",
    updateMessage: "保存成功",
    invalidateKeys: [departmentQueryKeys.all],
  });
}
