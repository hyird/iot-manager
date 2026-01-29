/**
 * 用户管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import { userQueryKeys } from "./keys";
import * as api from "./api";
import type { User } from "@/types";

/**
 * 创建用户
 */
export function useUserCreate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.create,
    onSuccess: () => {
      message.success("创建成功");
      queryClient.invalidateQueries({ queryKey: userQueryKeys.all });
    },
  });
}

/**
 * 更新用户
 */
export function useUserUpdate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: ({ id, data }: { id: number; data: User.UpdateDto }) => api.update(id, data),
    onSuccess: () => {
      message.success("更新成功");
      queryClient.invalidateQueries({ queryKey: userQueryKeys.all });
    },
  });
}

/**
 * 删除用户
 */
export function useUserDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: userQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/**
 * 保存用户（创建或更新）
 */
export function useUserSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (values: User.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, username: _, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    onSuccess: () => {
      message.success("保存成功");
      queryClient.invalidateQueries({ queryKey: userQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}
