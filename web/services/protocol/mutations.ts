/**
 * 协议配置 Mutation Hooks
 */

import type { Protocol } from "@/types";
import { useMutationWithFeedback, useSaveMutationWithFeedback } from "../common";
import { deviceKeys } from "../device/keys";
import * as api from "./api";
import { protocolQueryKeys } from "./keys";

/** 保存协议配置的参数类型（支持创建和更新） */
export type SaveProtocolConfigParams =
  | (Protocol.CreateDto & { id?: undefined })
  | (Protocol.UpdateDto & { id: number; protocol?: Protocol.Type });

/** 创建或更新协议配置 */
export function useProtocolConfigSave() {
  return useSaveMutationWithFeedback<
    SaveProtocolConfigParams,
    Protocol.CreateDto,
    Protocol.UpdateDto
  >({
    createFn: api.create,
    updateFn: (id, data) => api.update(id, data),
    toUpdatePayload: (data) => {
      const { id: _id, protocol: _protocol, ...rest } = data;
      return rest as Protocol.UpdateDto;
    },
    invalidateKeys: [protocolQueryKeys.all, deviceKeys.all],
  });
}

/** 删除协议配置 */
export function useProtocolConfigDelete() {
  return useMutationWithFeedback({
    mutationFn: api.remove,
    successMessage: "删除成功",
    invalidateKeys: [protocolQueryKeys.all, deviceKeys.all],
  });
}
