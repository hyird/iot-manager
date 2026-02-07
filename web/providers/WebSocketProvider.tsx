/**
 * WebSocket 实时推送 Provider
 *
 * 连接到后端 /ws 端点，接收领域事件并自动失效对应的 TanStack Query 缓存。
 * 当 WebSocket 连接时，页面轮询可被禁用以减少不必要的请求。
 */

import { useQueryClient } from "@tanstack/react-query";
import {
  createContext,
  type ReactNode,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
} from "react";
import { useAppSelector } from "@/store/hooks";

interface WsContextValue {
  connected: boolean;
}

const WsContext = createContext<WsContextValue>({ connected: false });

/** WebSocket 连接状态 hook */
export function useWsStatus() {
  return useContext(WsContext);
}

// WebSocket 事件类型 → TanStack Query key 前缀映射
const EVENT_QUERY_MAP: Record<string, string[][]> = {
  "device:created": [["device"]],
  "device:updated": [["device"]],
  "device:deleted": [["device"]],
  "device:realtime": [["device", "realtime"]],
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
};

/** 重连延迟（指数退避，最大 30 秒） */
function getReconnectDelay(attempt: number) {
  return Math.min(1000 * 2 ** attempt, 30000);
}

/** 心跳间隔（30 秒） */
const PING_INTERVAL = 30000;

export function WebSocketProvider({ children }: { children: ReactNode }) {
  const token = useAppSelector((s) => s.auth.token);
  const queryClient = useQueryClient();
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

        // 查找对应的 query key 并失效
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
  }, [token, queryClient]);

  useEffect(() => {
    if (token) {
      connect();
    }
    return cleanup;
  }, [token, connect, cleanup]);

  return <WsContext.Provider value={{ connected }}>{children}</WsContext.Provider>;
}
