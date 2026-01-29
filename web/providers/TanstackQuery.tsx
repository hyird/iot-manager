import type { ReactNode } from "react";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      refetchOnWindowFocus: true,
      refetchOnReconnect: true,
      retry: 0,
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
