/**
 * 链路管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import { linkQueryKeys } from "./keys";
import * as api from "./api";
import type { Link } from "@/types";

/**
 * 创建链路
 */
export function useLinkCreate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.create,
    onSuccess: () => {
      message.success("创建成功");
      queryClient.invalidateQueries({ queryKey: linkQueryKeys.all });
    },
  });
}

/**
 * 更新链路
 */
export function useLinkUpdate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: ({ id, data }: { id: number; data: Link.UpdateDto }) => api.update(id, data),
    onSuccess: () => {
      message.success("更新成功");
      queryClient.invalidateQueries({ queryKey: linkQueryKeys.all });
    },
  });
}

/**
 * 删除链路
 */
export function useLinkDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: linkQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/**
 * 保存链路（创建或更新）
 */
export function useLinkSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (values: Link.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    onSuccess: () => {
      message.success("保存成功");
      queryClient.invalidateQueries({ queryKey: linkQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}
