/**
 * 边缘节点 API
 */

import type { Agent } from "@/types";
import { request } from "../common";

const ENDPOINTS = {
  BASE: "/api/agent",
  OPTIONS: "/api/agent/options",
  EVENTS: (id: number) => `/api/agent/${id}/events`,
  DETAIL: (id: number) => `/api/agent/${id}`,
  APPROVE: (id: number) => `/api/agent/${id}/approve`,
  RESYNC: (id: number) => `/api/agent/${id}/resync`,
  NETWORK_CONFIG: (id: number) => `/api/agent/${id}/network-config`,
  ENDPOINTS_LIST: (agentId: number) => `/api/agent/${agentId}/endpoints`,
  ENDPOINT_DETAIL: (id: number) => `/api/agent/endpoints/${id}`,
} as const;

export function getList() {
  return request.get<Agent.Item[]>(ENDPOINTS.BASE);
}

export function getOptions() {
  return request.get<Agent.Item[]>(ENDPOINTS.OPTIONS);
}

export function getEvents(id: number, params?: { hours?: number; limit?: number }) {
  return request.get<Agent.Event[]>(ENDPOINTS.EVENTS(id), { params });
}

export function create(data: Agent.CreateInput) {
  return request.post<{ id: number; code: string; name: string }>(ENDPOINTS.BASE, data);
}

export function update(id: number, data: Agent.UpdateInput) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

export function approve(id: number) {
  return request.post<void>(ENDPOINTS.APPROVE(id));
}

export function resync(id: number) {
  return request.post<void>(ENDPOINTS.RESYNC(id));
}

export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}

export function updateNetworkConfig(id: number, data: Agent.NetworkConfigItem[]) {
  return request.put<void>(ENDPOINTS.NETWORK_CONFIG(id), { interfaces: data });
}

// ========== Agent Endpoint ==========

export function getEndpoints(agentId: number) {
  return request.get<Agent.Endpoint[]>(ENDPOINTS.ENDPOINTS_LIST(agentId));
}

export function createEndpoint(agentId: number, data: Agent.EndpointCreate) {
  return request.post<{ id: number }>(ENDPOINTS.ENDPOINTS_LIST(agentId), data);
}

export function updateEndpoint(id: number, data: Agent.EndpointUpdate) {
  return request.put<void>(ENDPOINTS.ENDPOINT_DETAIL(id), data);
}

export function removeEndpoint(id: number) {
  return request.delete<void>(ENDPOINTS.ENDPOINT_DETAIL(id));
}
