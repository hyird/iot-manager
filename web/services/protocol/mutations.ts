/**
 * 协议配置 Mutation Hooks
 */

import type { Protocol } from "@/types";
import { useMutationWithFeedback } from "../common";
import { deviceKeys } from "../device/keys";
import * as api from "./api";
import { protocolQueryKeys } from "./keys";

/** 保存协议配置的参数类型（支持创建和更新） */
export type SaveProtocolConfigParams =
  | (Protocol.CreateDto & { id?: undefined })
  | (Protocol.UpdateDto & { id: number; protocol?: Protocol.Type });

/** 创建或更新协议配置 */
export function useProtocolConfigSave() {
  return useMutationWithFeedback({
    mutationFn: async (data: SaveProtocolConfigParams) => {
      if (data.id) {
        const { id, protocol: _protocol, ...rest } = data;
        await api.update(id, rest);
        return;
      }
      await api.create(data as Protocol.CreateDto);
    },
    successMessage: (_, variables) => (variables.id ? "更新成功" : "创建成功"),
    invalidateKeys: [protocolQueryKeys.all, deviceKeys.all],
  });
}

/** 删除协议配置 */
export function useProtocolConfigDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    errorMessage: (e) => e.message || "删除失败",
    invalidateKeys: [protocolQueryKeys.all, deviceKeys.all],
  });
}
