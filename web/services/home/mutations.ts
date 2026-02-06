/**
 * 首页 Mutations
 */

import { useMutation } from "@tanstack/react-query";
import * as api from "./api";

/** 清理所有缓存 */
export function useClearCache() {
  return useMutation({
    mutationFn: api.clearCache,
  });
}
