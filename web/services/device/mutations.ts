/**
 * 设备管理 Mutation Hooks
 */

import type { Device } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as deviceApi from "./api";
import { deviceKeys } from "./keys";

/** 保存设备 Mutation (创建或更新) */
export function useDeviceSave() {
  return useSaveMutationWithFeedback<
    Device.CreateDto & { id?: number },
    Device.CreateDto,
    Device.UpdateDto
  >({
    createFn: deviceApi.create,
    updateFn: deviceApi.update,
    toUpdatePayload: ({ id: _id, ...rest }) => rest,
    invalidateKeys: [deviceKeys.all],
  });
}

/** 删除设备 Mutation */
export function useDeviceDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => deviceApi.remove(id),
    successMessage: "删除成功",
    invalidateKeys: [deviceKeys.all],
  });
}

/** 下发设备指令 Mutation */
export function useDeviceCommand() {
  return useMutationWithFeedback({
    mutationFn: (data: { linkId: number; payload: Device.Command }) =>
      deviceApi.sendCommand(data.linkId, data.payload),
    successMessage: "指令下发成功，设备已应答",
  });
}

/** 新增或更新设备分享权限 Mutation */
export function useDeviceShareSave() {
  return useMutationWithFeedback({
    mutationFn: (data: { deviceId: number; payload: Device.ShareUpsertDto }) =>
      deviceApi.upsertShare(data.deviceId, data.payload),
    successMessage: "分享权限已更新",
    invalidateKeys: [deviceKeys.all],
  });
}

/** 删除设备分享权限 Mutation */
export function useDeviceShareDelete() {
  return useMutationWithFeedback({
    mutationFn: (data: { deviceId: number; targetType: string; targetId: number }) =>
      deviceApi.removeShare(data.deviceId, data.targetType, data.targetId),
    successMessage: "已取消分享",
    invalidateKeys: [deviceKeys.all],
  });
}
