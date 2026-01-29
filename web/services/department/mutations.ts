/**
 * 部门管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import { departmentQueryKeys } from "./keys";
import * as api from "./api";
import type { Department } from "@/types";

/**
 * 创建部门
 */
export function useDepartmentCreate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.create,
    onSuccess: () => {
      message.success("创建成功");
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
  });
}

/**
 * 更新部门
 */
export function useDepartmentUpdate() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: ({ id, data }: { id: number; data: Department.UpdateDto }) => api.update(id, data),
    onSuccess: () => {
      message.success("更新成功");
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
  });
}

/**
 * 删除部门
 */
export function useDepartmentDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/**
 * 保存部门（创建或更新）
 */
export function useDepartmentSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (values: Department.CreateDto & { id?: number }) => {
      if (values.id) {
        const { id, ...data } = values;
        return api.update(id, data);
      }
      return api.create(values);
    },
    onSuccess: () => {
      message.success("保存成功");
      queryClient.invalidateQueries({ queryKey: departmentQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}
