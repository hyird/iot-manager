/**
 * 菜单管理 Mutation Hooks
 */

import type { Menu } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as api from "./api";
import { menuQueryKeys } from "./keys";

/** 创建菜单 */
export function useMenuCreate() {
  return useMutationWithFeedback({
    mutationFn: api.create,
    successMessage: "创建成功",
    invalidateKeys: [menuQueryKeys.all],
  });
}

/** 更新菜单 */
export function useMenuUpdate() {
  return useMutationWithFeedback({
    mutationFn: ({ id, data }: { id: number; data: Menu.UpdateDto }) => api.update(id, data),
    successMessage: "更新成功",
    invalidateKeys: [menuQueryKeys.all],
  });
}

/** 删除菜单 */
export function useMenuDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [menuQueryKeys.all],
  });
}

/** 保存菜单（创建或更新） */
export function useMenuSave() {
  return useMutationWithFeedback({
    mutationFn: async (values: Menu.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    successMessage: "保存成功",
    invalidateKeys: [menuQueryKeys.all],
  });
}
