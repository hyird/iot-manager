import { useMemo } from "react";
import { useCurrentUser } from "@/services";

/**
 * 权限检查 Hook
 * 从 TanStack Query 缓存的用户数据中派生权限
 */
export function usePermission(code?: string): boolean {
  const { data: user } = useCurrentUser();

  const { isSuperAdmin, codesSet } = useMemo(() => {
    if (!user) return { isSuperAdmin: false, codesSet: new Set<string>() };

    const superAdmin = user.roles?.some((r) => r.code === "superadmin") ?? false;
    const codes = new Set<string>();
    user.menus?.forEach((m) => {
      if (m.type === "button" && m.permission_code) {
        codes.add(m.permission_code);
      }
    });
    return { isSuperAdmin: superAdmin, codesSet: codes };
  }, [user]);

  if (!code) return true;
  if (isSuperAdmin) return true;
  return codesSet.has(code);
}
