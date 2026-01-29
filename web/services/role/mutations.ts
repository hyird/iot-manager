/**
 * 角色管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import { roleQueryKeys } from "./keys";
import * as api from "./api";
import type { Role } from "@/types";

/**
 * 创建角色
 */
export function useRoleCreate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.create,
    onSuccess: () => {
      message.success("创建成功");
      queryClient.invalidateQueries({ queryKey: roleQueryKeys.all });
    },
  });
}

/**
 * 更新角色
 */
export function useRoleUpdate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: ({ id, data }: { id: number; data: Role.UpdateDto }) => api.update(id, data),
    onSuccess: () => {
      message.success("更新成功");
      queryClient.invalidateQueries({ queryKey: roleQueryKeys.all });
    },
  });
}

/**
 * 删除角色
 */
export function useRoleDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: roleQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/**
 * 保存角色（创建或更新）
 */
export function useRoleSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (values: Role.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    onSuccess: () => {
      message.success("保存成功");
      queryClient.invalidateQueries({ queryKey: roleQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}
