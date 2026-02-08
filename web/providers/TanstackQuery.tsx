import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import type { ReactNode } from "react";

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      staleTime: 5 * 60 * 1000, // 5 分钟（WebSocket 推送保持数据新鲜，无需频繁 refetch）
      gcTime: 10 * 60 * 1000, // 10 分钟后清理未使用的缓存
      refetchOnWindowFocus: false, // 禁用窗口焦点 refetch（由 WebSocket 事件驱动更新）
      refetchOnReconnect: true,
      retry: 1,
    },
    mutations: {
      retry: 0,
    },
  },
});

export interface TanstackQueryProps {
  children: ReactNode;
}

export function TanstackQuery({ children }: TanstackQueryProps) {
  return <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>;
}
