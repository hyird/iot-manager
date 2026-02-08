import type { DeviceGroup } from "@/types";
import { useMutationWithFeedback } from "../common";
import * as deviceGroupApi from "./api";
import { deviceGroupKeys } from "./keys";

export function useDeviceGroupSave() {
  return useMutationWithFeedback({
    mutationFn: async (data: DeviceGroup.CreateDto & { id?: number }) => {
      if (data.id) {
        const { id, ...rest } = data;
        await deviceGroupApi.update(id, rest);
      } else {
        await deviceGroupApi.create(data);
      }
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
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
