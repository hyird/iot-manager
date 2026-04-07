/**
 * 设备管理 Query Keys
 */

import type { Device } from "@/types";
import { createQueryKeys } from "../common";

const deviceBaseKeys = createQueryKeys("device");

export const deviceKeys = {
  ...deviceBaseKeys,
  list: (params?: Device.Query) => [...deviceBaseKeys.lists(), params] as const,
  static: () => [...deviceBaseKeys.all, "static"] as const,
  realtime: () => [...deviceBaseKeys.all, "realtime"] as const,
  history: (params?: Device.HistoryQuery) => [...deviceBaseKeys.all, "history", params] as const,
};
