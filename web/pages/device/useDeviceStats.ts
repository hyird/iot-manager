/**
 * 设备统计 Hook
 */

import { useMemo } from "react";
import type { Device } from "@/types";
import { isOnline } from "./utils";

export interface DeviceStats {
  total: number;
  online: number;
  offline: number;
  enabled: number;
  byProtocol: Record<string, { total: number; online: number; offline: number; enabled: number }>;
}

export function useDeviceStats(deviceList: Device.RealTimeData[]): DeviceStats {
  return useMemo(() => {
    const total = deviceList.length;
    let online = 0;
    let offline = 0;
    let enabled = 0;
    const byProtocol: DeviceStats["byProtocol"] = {};

    deviceList.forEach((d) => {
      const isDeviceOnline = isOnline(d.lastHeartbeatTime, d.reportTime, d.online_timeout);
      if (isDeviceOnline) online++;
      else offline++;
      if (d.status === "enabled") enabled++;

      const protocolName = d.protocol_type || d.protocol_name || "未知";
      if (!byProtocol[protocolName]) {
        byProtocol[protocolName] = { total: 0, online: 0, offline: 0, enabled: 0 };
      }
      byProtocol[protocolName].total++;
      if (isDeviceOnline) byProtocol[protocolName].online++;
      else byProtocol[protocolName].offline++;
      if (d.status === "enabled") byProtocol[protocolName].enabled++;
    });

    return { total, online, offline, enabled, byProtocol };
  }, [deviceList]);
}
