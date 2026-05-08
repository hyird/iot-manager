import { type UseQueryOptions, useQuery } from "@tanstack/react-query";
import type { GB28181 } from "@/types";
import * as api from "./api";
import { gb28181Keys } from "./keys";

export function useGb28181Health(
  options?: Omit<UseQueryOptions<GB28181.Health>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: gb28181Keys.health(),
    queryFn: api.getHealth,
    refetchInterval: 10000,
    retry: false,
    ...options,
  });
}

export function useGb28181SipConfig(
  options?: Omit<UseQueryOptions<GB28181.SipConfig>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: gb28181Keys.sipConfig(),
    queryFn: api.getSipConfig,
    staleTime: 60 * 1000,
    ...options,
  });
}

export function useGb28181Devices(
  options?: Omit<UseQueryOptions<GB28181.Items<GB28181.Device>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: gb28181Keys.devices(),
    queryFn: api.getDevices,
    refetchInterval: 3000,
    refetchIntervalInBackground: false,
    ...options,
  });
}

export function useGb28181Streams(
  options?: Omit<UseQueryOptions<GB28181.Items<GB28181.StreamStatus>>, "queryKey" | "queryFn">
) {
  return useQuery({
    queryKey: gb28181Keys.streams(),
    queryFn: api.getStreams,
    refetchInterval: 3000,
    refetchIntervalInBackground: false,
    ...options,
  });
}
