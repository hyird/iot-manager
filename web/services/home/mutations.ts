/**
 * 首页 Mutations
 */

import { useMutation, useQueryClient } from "@tanstack/react-query";
import * as api from "./api";

/** 清理所有缓存 */
export function useClearCache() {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: api.clearCache,
    onSuccess: () => {
      queryClient.invalidateQueries();
    },
  });
}
