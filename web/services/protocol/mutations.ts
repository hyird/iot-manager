/**
 * 协议配置 Mutation Hooks
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import type { Protocol } from "@/types";
import { protocolQueryKeys } from "./keys";
import * as api from "./api";

/** 保存协议配置的参数类型（支持创建和更新） */
export type SaveProtocolConfigParams =
  | (Protocol.CreateDto & { id?: undefined })
  | (Protocol.UpdateDto & { id: number; protocol?: Protocol.Type });

/** 创建或更新协议配置 */
export function useProtocolConfigSave() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: async (data: SaveProtocolConfigParams) => {
      if (data.id) {
        const { id, protocol: _protocol, ...rest } = data;
        await api.update(id, rest);
        return;
      }
      await api.create(data as Protocol.CreateDto);
    },
    onSuccess: (_, variables) => {
      message.success(variables.id ? "更新成功" : "创建成功");
      queryClient.invalidateQueries({ queryKey: protocolQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "操作失败");
    },
  });
}

/** 删除协议配置 */
export function useProtocolConfigDelete() {
  const queryClient = useQueryClient();
  const { message } = App.useApp();

  return useMutation({
    mutationFn: api.remove,
    onSuccess: () => {
      message.success("删除成功");
      queryClient.invalidateQueries({ queryKey: protocolQueryKeys.all });
    },
    onError: (error: Error) => {
      message.error(error.message || "删除失败");
    },
  });
}
