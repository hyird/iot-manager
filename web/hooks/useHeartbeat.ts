import { useEffect, useRef } from "react";
import { useAuthStore } from "@/store/hooks";

interface UseHeartbeatOptions {
  /** 心跳间隔(毫秒),默认 5 分钟 */
  interval?: number;
  /** 是否启用,默认 true */
  enabled?: boolean;
}

/**
 * 心跳检测 Hook
 * 定期刷新用户信息,保持会话活跃
 */
export function useHeartbeat(options: UseHeartbeatOptions = {}) {
  const { interval = 5 * 60 * 1000, enabled = true } = options;
  const { refreshUser, token } = useAuthStore();
  const timerRef = useRef<NodeJS.Timeout>(undefined);

  useEffect(() => {
    if (!enabled || !token) {
      return;
    }

    const startHeartbeat = () => {
      timerRef.current = setInterval(async () => {
        try {
          await refreshUser().unwrap();
        } catch (error) {
          console.error("[心跳] 刷新失败:", error);
        }
      }, interval);
    };

    startHeartbeat();

    return () => {
      if (timerRef.current) {
        clearInterval(timerRef.current);
      }
    };
  }, [enabled, token, interval, refreshUser]);
}
