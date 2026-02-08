/**
 * Agent Mutation Hooks
 */

import type { Agent } from "@/types";
import { useMutationWithFeedback } from "../common";
import { linkQueryKeys } from "../link/keys";
import * as api from "./api";
import { agentQueryKeys } from "./keys";

export function useAgentCreate() {
  return useMutationWithFeedback<{ id: number; code: string; name: string }, Agent.CreateInput>({
    mutationFn: api.create,
    successMessage: "Agent 节点创建成功",
    invalidateKeys: [agentQueryKeys.all],
  });
}

export function useAgentUpdate() {
  return useMutationWithFeedback<void, { id: number; data: Agent.UpdateInput }>({
    mutationFn: ({ id, data }) => api.update(id, data),
    successMessage: "Agent 节点更新成功",
    invalidateKeys: [agentQueryKeys.all],
  });
}

export function useAgentResync() {
  return useMutationWithFeedback<void, number>({
    mutationFn: api.resync,
    successMessage: "已发送配置重同步请求",
    invalidateKeys: [agentQueryKeys.all, linkQueryKeys.all],
  });
}

export function useAgentDelete() {
  return useMutationWithFeedback<void, number>({
    mutationFn: api.remove,
    successMessage: "采集 Agent 已删除",
    invalidateKeys: [agentQueryKeys.all, linkQueryKeys.all],
  });
}

export function useAgentNetworkConfig() {
  return useMutationWithFeedback<void, { id: number; data: Agent.NetworkConfigItem[] }>({
    mutationFn: ({ id, data }) => api.updateNetworkConfig(id, data),
    successMessage: "网络配置已保存并下发",
    invalidateKeys: [agentQueryKeys.all],
  });
}

// ========== Agent Endpoint Mutations ==========

export function useAgentEndpointCreate() {
  return useMutationWithFeedback<{ id: number }, { agentId: number; data: Agent.EndpointCreate }>({
    mutationFn: ({ agentId, data }) => api.createEndpoint(agentId, data),
    successMessage: "端点创建成功",
    invalidateKeys: [agentQueryKeys.all],
  });
}

export function useAgentEndpointUpdate() {
  return useMutationWithFeedback<void, { id: number; data: Agent.EndpointUpdate }>({
    mutationFn: ({ id, data }) => api.updateEndpoint(id, data),
    successMessage: "端点更新成功",
    invalidateKeys: [agentQueryKeys.all],
  });
}

export function useAgentEndpointDelete() {
  return useMutationWithFeedback<void, number>({
    mutationFn: api.removeEndpoint,
    successMessage: "端点已删除",
    invalidateKeys: [agentQueryKeys.all],
  });
}
