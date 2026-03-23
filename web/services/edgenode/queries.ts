/**
 * EdgeNode Query Hooks
 */

import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { Agent } from "@/types";
import * as api from "./api";
import { agentQueryKeys } from "./keys";

export function useAgentList(
  options?: Omit<UseQueryOptions<Agent.Item[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: agentQueryKeys.list(),
    queryFn: api.getList,
    staleTime: 10 * 1000,
    ...options,
  });
}

export function useAgentOptions(
  options?: Omit<UseQueryOptions<Agent.Item[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: agentQueryKeys.options(),
    queryFn: api.getOptions,
    staleTime: 30 * 1000,
    ...options,
  });
}

export function useAgentEvents(
  id?: number,
  params?: { hours?: number; limit?: number },
  options?: Omit<UseQueryOptions<Agent.Event[]>, "queryKey" | "queryFn">
) {
  const hours = params?.hours ?? 24;
  const limit = params?.limit ?? 100;
  return useQuery({
    queryKey: agentQueryKeys.events(id ?? 0, hours, limit),
    queryFn: () => {
      if (!id) {
        return Promise.resolve([] as Agent.Event[]);
      }
      return api.getEvents(id, { hours, limit });
    },
    enabled: !!id && (options?.enabled ?? true),
    staleTime: 10 * 1000,
    ...options,
  });
}

export function useAgentEndpoints(
  agentId?: number,
  options?: Omit<UseQueryOptions<Agent.Endpoint[]>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: agentQueryKeys.endpoints(agentId ?? 0),
    queryFn: () => {
      if (!agentId) return Promise.resolve([] as Agent.Endpoint[]);
      return api.getEndpoints(agentId);
    },
    enabled: !!agentId && (options?.enabled ?? true),
    staleTime: 10 * 1000,
    ...options,
  });
}
