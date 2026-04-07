/**
 * 菜单管理 Mutation Hooks
 */

import type { Menu } from "@/types";
import { useSaveMutationWithFeedback } from "../common";
import * as api from "./api";
import { menuQueryKeys } from "./keys";

/** 保存菜单（创建或更新） */
export function useMenuSave() {
  return useSaveMutationWithFeedback<
    Menu.CreateDto & { id?: number },
    Menu.CreateDto,
    Menu.UpdateDto
  >({
    createFn: api.create,
    updateFn: api.update,
    toUpdatePayload: ({ id: _id, ...data }) => data,
    createMessage: "保存成功",
    updateMessage: "保存成功",
    invalidateKeys: [menuQueryKeys.all],
  });
}
