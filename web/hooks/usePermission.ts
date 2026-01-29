import { usePermission as usePermissionHook } from "@/store/hooks";

export function usePermission(code?: string) {
  return usePermissionHook(code);
}
