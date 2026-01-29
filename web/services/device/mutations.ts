/**
 * 设备管理 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import type { Device } from "@/types";
import { deviceKeys } from "./keys";
import * as deviceApi from "./api";

/** 保存设备 Mutation (创建或更新) */
export function useDeviceSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (data: Device.CreateDto & { id?: number }): Promise<void> => {
      if (data.id) {
        const { id, ...rest } = data;
        await deviceApi.update(id, rest);
      } else {
        await deviceApi.create(data);
      }
    },
    onSuccess: (_, variables) => {
      message.success(variables.id ? "更新成功" : "创建成功");
      queryClient.invalidateQueries({ queryKey: deviceKeys.lists() });
      if (variables.id) {
        queryClient.invalidateQueries({ queryKey: deviceKeys.detail(variables.id) });
      }
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}

/** 删除设备 Mutation */
export function useDeviceDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: (id: number) => deviceApi.remove(id),
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: deviceKeys.lists() });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}

/** 下发设备指令 Mutation */
export function useDeviceCommand() {
  const { message } = App.useApp();

  return useMutation({
    mutationFn: (data: { linkId: number; payload: Device.Command }) =>
      deviceApi.sendCommand(data.linkId, data.payload),
    onSuccess: () => {
      message.success("指令下发成功，设备已应答");
    },
    onError: (error: Error) => {
      message.error(error.message || "指令下发失败");
    },
  });
}
