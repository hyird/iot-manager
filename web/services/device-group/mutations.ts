import type { DeviceGroup } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import * as deviceGroupApi from "./api";
import { deviceGroupKeys } from "./keys";

export function useDeviceGroupSave() {
  return useSaveMutationWithFeedback<
    DeviceGroup.CreateDto & { id?: number },
    DeviceGroup.CreateDto,
    DeviceGroup.UpdateDto
  >({
    createFn: deviceGroupApi.create,
    updateFn: deviceGroupApi.update,
    toUpdatePayload: ({ id: _id, ...rest }) => rest,
    invalidateKeys: [deviceGroupKeys.all],
  });
}

export function useDeviceGroupDelete() {
  return useMutationWithFeedback({
    mutationFn: (id: number) => deviceGroupApi.remove(id),
    successMessage: "删除成功",
    invalidateKeys: [deviceGroupKeys.all],
  });
}
