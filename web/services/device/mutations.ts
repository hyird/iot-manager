/**
 * 设备管理 Mutation Hooks
 */

import { useQueryClient } from "@tanstack/react-query";
import type { Device } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as deviceApi from "./api";
import { deviceKeys } from "./keys";

/** 保存设备 Mutation (创建或更新) */
export function useDeviceSave() {
  const queryClient = useQueryClient();

  return useMutationWithFeedback({
    mutationFn: async (data: Device.CreateDto & { id?: number }): Promise<void> => {
      if (data.id) {
        const { id, ...rest } = data;
        await deviceApi.update(id, rest);
      } else {
        await deviceApi.create(data);
      }
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
    invalidateKeys: [deviceKeys.all],
    onSuccess: (_, variables) => {
      if (variables.id) {
        queryClient.invalidateQueries({ queryKey: deviceKeys.detail(variables.id) });
      }
    },
  });
}

/** 删除设备 Mutation */
export function useDeviceDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => deviceApi.remove(id),
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [deviceKeys.all],
  });
}

/** 下发设备指令 Mutation */
export function useDeviceCommand() {
  return useMutationWithFeedback({
    mutationFn: (data: { linkId: number; payload: Device.Command }) =>
      deviceApi.sendCommand(data.linkId, data.payload),
    successMessage: "指令下发成功，设备已应答",
    errorMessage: (e) => e.message || "指令下发失败",
  });
}
