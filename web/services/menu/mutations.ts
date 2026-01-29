/**
 * 菜单管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import { menuQueryKeys } from "./keys";
import * as api from "./api";
import type { Menu } from "@/types";

/**
 * 创建菜单
 */
export function useMenuCreate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.create,
    onSuccess: () => {
      message.success("创建成功");
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
    },
  });
}

/**
 * 更新菜单
 */
export function useMenuUpdate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: ({ id, data }: { id: number; data: Menu.UpdateDto }) => api.update(id, data),
    onSuccess: () => {
      message.success("更新成功");
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
    },
  });
}

/**
 * 删除菜单
 */
export function useMenuDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/**
 * 保存菜单（创建或更新）
 */
export function useMenuSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (values: Menu.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    onSuccess: () => {
      message.success("保存成功");
      queryClient.invalidateQueries({ queryKey: menuQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}
