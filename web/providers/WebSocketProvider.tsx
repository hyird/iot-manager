/**
 * WebSocket 实时推送 Provider
 *
 * 连接到后端 /ws 端点，接收领域事件：
 * - device:realtime → 直接 setQueryData 更新缓存（零 HTTP 请求）
 * - 其他事件 → invalidateQueries 触发 refetch
 *
 * 降级策略：WS 断开时 connected=false，页面自动恢复轮询。
 */

import { useQueryClient } from "@tanstack/react-query";
import { App } from "antd";
import {
  createContext,
  type ReactNode,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
} from "react";
import { deviceKeys } from "@/services/device/keys";
import { useAppSelector } from "@/store/hooks";
import type { Device } from "@/types";

interface WsContextValue {
  connected: boolean;
}

const WsContext = createContext<WsContextValue>({ connected: false });

/** WebSocket 连接状态 hook */
export function useWsStatus() {
  return useContext(WsContext);
}

// WebSocket 事件类型 → TanStack Query key 前缀映射（device:realtime 单独处理）
const EVENT_QUERY_MAP: Record<string, string[][]> = {
  "device:created": [["device"]],
  "device:updated": [["device"]],
  "device:deleted": [["device"]],
  "link:created": [["links"]],
  "link:updated": [["links"]],
  "link:deleted": [["links"]],
  "link:connection": [["links"]],
  "protocol:created": [["protocol"]],
  "protocol:updated": [["protocol"]],
  "protocol:deleted": [["protocol"]],
  "system:user": [["users"]],
  "system:role": [["roles"]],
  "system:menu": [["menus"]],
  "system:department": [["departments"]],
  "alert:rule:created": [["alert"]],
  "alert:rule:updated": [["alert"]],
  "alert:rule:deleted": [["alert"]],
  "alert:resolved": [["alert"]],
  "monitor:updated": [["home", "monitor"]], // 监控数据更新
  "stats:updated": [["home", "stats"]], // 统计数据更新
};

/** 重连延迟（指数退避，最大 30 秒） */
function getReconnectDelay(attempt: number) {
  return Math.min(1000 * 2 ** attempt, 30000);
}

/** 心跳间隔（30 秒） */
const PING_INTERVAL = 30000;

/** device:realtime WS payload 中的单个设备更新 */
interface RealtimeUpdate {
  id: number;
  reportTime?: string | null;
  lastHeartbeatTime?: string | null;
  elements?: Device.Element[];
  image?: { funcCode: string; data: string } | null;
}

export function WebSocketProvider({ children }: { children: ReactNode }) {
  const token = useAppSelector((s) => s.auth.token);
  const queryClient = useQueryClient();
  const { notification } = App.useApp();
  const [connected, setConnected] = useState(false);

  const wsRef = useRef<WebSocket | null>(null);
  const reconnectAttemptRef = useRef(0);
  const reconnectTimerRef = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const pingTimerRef = useRef<ReturnType<typeof setInterval> | undefined>(undefined);

  const cleanup = useCallback(() => {
    if (pingTimerRef.current) {
      clearInterval(pingTimerRef.current);
      pingTimerRef.current = undefined;
    }
    if (reconnectTimerRef.current) {
      clearTimeout(reconnectTimerRef.current);
      reconnectTimerRef.current = undefined;
    }
    if (wsRef.current) {
      wsRef.current.onclose = null; // 避免触发重连
      wsRef.current.close();
      wsRef.current = null;
    }
    setConnected(false);
  }, []);

  /** 将 WS 推送的实时数据直接合并到 Query 缓存（带变更检测） */
  const mergeRealtimeUpdates = useCallback(
    (updates: RealtimeUpdate[]) => {
      const queryKey = deviceKeys.realtime();
      const existing = queryClient.getQueryData<{ list: Device.Realtime[] }>(queryKey);

      if (!existing?.list?.length) {
        queryClient.setQueryData(queryKey, { list: updates as Device.Realtime[] });
        return;
      }

      const updateMap = new Map(updates.map((u) => [u.id, u]));
      let hasChanges = false;

      const mergedList = existing.list.map((device) => {
        const update = updateMap.get(device.id);
        if (!update) return device;

        updateMap.delete(device.id);

        // 变更检测：仅在数据确实变化时创建新对象（避免不必要的重渲染）
        if (
          device.reportTime === (update.reportTime ?? device.reportTime) &&
          device.lastHeartbeatTime === (update.lastHeartbeatTime ?? device.lastHeartbeatTime) &&
          !update.elements &&
          !update.image
        ) {
          return device;
        }

        hasChanges = true;
        return {
          ...device,
          reportTime: update.reportTime ?? device.reportTime,
          lastHeartbeatTime: update.lastHeartbeatTime ?? device.lastHeartbeatTime,
          elements: update.elements ?? device.elements,
          image: update.image ?? device.image,
        } as Device.Realtime;
      });

      if (updateMap.size > 0) {
        hasChanges = true;
        for (const update of updateMap.values()) {
          mergedList.push(update as Device.Realtime);
        }
      }

      // 仅在真正有变化时更新缓存，避免无变更时触发组件重渲染
      if (hasChanges) {
        queryClient.setQueryData(queryKey, { list: mergedList });
      }
    },
    [queryClient]
  );

  const connect = useCallback(() => {
    if (!token) return;

    const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = `${protocol}//${window.location.host}/ws?token=${encodeURIComponent(token)}`;

    const ws = new WebSocket(url);
    wsRef.current = ws;

    ws.onopen = () => {
      reconnectAttemptRef.current = 0;

      // 应用层心跳
      pingTimerRef.current = setInterval(() => {
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(JSON.stringify({ type: "ping" }));
        }
      }, PING_INTERVAL);
    };

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data) as { type: string; data?: unknown };

        // 连接确认
        if (msg.type === "connected") {
          setConnected(true);
          return;
        }

        // 心跳回复 — 忽略
        if (msg.type === "pong") return;

        // 错误消息
        if (msg.type === "error") return;

        // alert:triggered → 弹出通知 + 刷新告警缓存
        if (msg.type === "alert:triggered") {
          const p = msg.data as {
            severity: string;
            message: string;
            deviceName: string;
          };
          notification[p.severity === "critical" ? "error" : "warning"]({
            message: p.severity === "critical" ? "严重告警" : "告警通知",
            description: `${p.deviceName}: ${p.message}`,
            duration: p.severity === "critical" ? 0 : 5,
          });
          queryClient.invalidateQueries({ queryKey: ["alert"] });
          return;
        }

        // alert:resolved → 恢复通知 + 刷新告警缓存
        if (msg.type === "alert:resolved") {
          const p = msg.data as {
            recordId: number;
            reason: string;
          };
          notification.info({
            message: "告警已恢复",
            description: `告警记录 #${p.recordId} 已${p.reason}`,
            duration: 3,
          });
          queryClient.invalidateQueries({ queryKey: ["alert"] });
          return;
        }

        // device:realtime → 直接更新缓存，零 HTTP 请求
        if (msg.type === "device:realtime") {
          const payload = msg.data as { updates?: RealtimeUpdate[] } | undefined;
          if (payload?.updates?.length) {
            mergeRealtimeUpdates(payload.updates);
          }
          return;
        }

        // device:offline → 清空离线设备的 reportTime，即时反映离线状态
        if (msg.type === "device:offline") {
          const payload = msg.data as { deviceIds?: number[] } | undefined;
          if (payload?.deviceIds?.length) {
            const offlineSet = new Set(payload.deviceIds);
            const queryKey = deviceKeys.realtime();
            const existing = queryClient.getQueryData<{ list: Device.Realtime[] }>(queryKey);
            if (existing?.list?.length) {
              let hasChanges = false;
              const mergedList = existing.list.map((device) => {
                if (!offlineSet.has(device.id) || !device.reportTime) return device;
                hasChanges = true;
                return { ...device, reportTime: null };
              });
              if (hasChanges) {
                queryClient.setQueryData(queryKey, { list: mergedList });
              }
            }
          }
          return;
        }

        // 其他事件：invalidate 触发 refetch
        const keys = EVENT_QUERY_MAP[msg.type];
        if (keys) {
          for (const key of keys) {
            queryClient.invalidateQueries({ queryKey: key });
          }
        }
      } catch {
        // JSON 解析失败，忽略
      }
    };

    ws.onclose = () => {
      setConnected(false);
      if (pingTimerRef.current) {
        clearInterval(pingTimerRef.current);
        pingTimerRef.current = undefined;
      }

      // 指数退避重连
      const delay = getReconnectDelay(reconnectAttemptRef.current);
      reconnectAttemptRef.current++;
      reconnectTimerRef.current = setTimeout(connect, delay);
    };

    ws.onerror = () => {
      // onclose 会紧随 onerror 触发，重连逻辑在 onclose 中处理
    };
  }, [token, queryClient, notification, mergeRealtimeUpdates]);

  useEffect(() => {
    if (token) {
      connect();
    }
    return cleanup;
  }, [token, connect, cleanup]);

  return <WsContext.Provider value={{ connected }}>{children}</WsContext.Provider>;
}
