/**
 * 设备管理 Query Keys
 */

import type { Device } from "@/types";

export const deviceKeys = {
  all: ["device"] as const,
  lists: () => [...deviceKeys.all, "list"] as const,
  list: (params?: Device.Query) => [...deviceKeys.lists(), params] as const,
  static: () => [...deviceKeys.all, "static"] as const,
  realtime: () => [...deviceKeys.all, "realtime"] as const,
  details: () => [...deviceKeys.all, "detail"] as const,
  detail: (id: number) => [...deviceKeys.details(), id] as const,
  options: () => [...deviceKeys.all, "options"] as const,
  history: (params?: Device.HistoryQuery) => [...deviceKeys.all, "history", params] as const,
};
