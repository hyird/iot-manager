import type { UseQueryOptions } from "@tanstack/react-query";
import { useQuery } from "@tanstack/react-query";
import type { DeviceGroup } from "@/types";
import * as deviceGroupApi from "./api";
import { deviceGroupKeys } from "./keys";

export function useDeviceGroupTree(options?: Partial<UseQueryOptions<DeviceGroup.TreeItem[]>>) {
  return useQuery({
    queryKey: deviceGroupKeys.tree(),
    queryFn: () => deviceGroupApi.getTree(),
    staleTime: 5 * 60 * 1000,
    ...options,
  });
}

export function useDeviceGroupTreeWithCount(
  options?: Partial<UseQueryOptions<DeviceGroup.TreeItem[]>>
) {
  return useQuery({
    queryKey: deviceGroupKeys.treeWithCount(),
    queryFn: () => deviceGroupApi.getTreeWithCount(),
    staleTime: 2 * 60 * 1000,
    ...options,
  });
}
