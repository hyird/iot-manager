import type { DeviceGroup } from "@/types";
import { request } from "../common";

const ENDPOINTS = {
  BASE: "/api/device-groups",
  DETAIL: (id: number) => `/api/device-groups/${id}`,
  TREE: "/api/device-groups/tree",
  TREE_COUNT: "/api/device-groups/tree-count",
} as const;

export function getTree(status?: DeviceGroup.Status) {
  return request.get<DeviceGroup.TreeItem[]>(ENDPOINTS.TREE, {
    params: status ? { status } : undefined,
  });
}

export function getTreeWithCount() {
  return request.get<DeviceGroup.TreeItem[]>(ENDPOINTS.TREE_COUNT);
}

export function getDetail(id: number) {
  return request.get<DeviceGroup.Item>(ENDPOINTS.DETAIL(id));
}

export function create(data: DeviceGroup.CreateDto) {
  return request.post<void>(ENDPOINTS.BASE, data);
}

export function update(id: number, data: DeviceGroup.UpdateDto) {
  return request.put<void>(ENDPOINTS.DETAIL(id), data);
}

export function remove(id: number) {
  return request.delete<void>(ENDPOINTS.DETAIL(id));
}
