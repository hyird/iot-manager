/**
 * 部门管理 Mutation Hooks
 */

import type { Department } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { departmentQueryKeys } from "./keys";

/** 创建部门 */
export function useDepartmentCreate() {
  return useMutationWithFeedback({
    mutationFn: api.create,
    successMessage: "创建成功",
    invalidateKeys: [departmentQueryKeys.all],
  });
}

/** 更新部门 */
export function useDepartmentUpdate() {
  return useMutationWithFeedback({
    mutationFn: ({ id, data }: { id: number; data: Department.UpdateDto }) => api.update(id, data),
    successMessage: "更新成功",
    invalidateKeys: [departmentQueryKeys.all],
  });
}

/** 删除部门 */
export function useDepartmentDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [departmentQueryKeys.all],
  });
}

/** 保存部门（创建或更新） */
export function useDepartmentSave() {
  return useMutationWithFeedback({
    mutationFn: async (values: Department.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    successMessage: "保存成功",
    invalidateKeys: [departmentQueryKeys.all],
  });
}
