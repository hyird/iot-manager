import { useCallback, useMemo } from "react";
import { useCurrentUser } from "@/services";

/** 内部共享的权限状态 */
function usePermissionState() {
  const { data: user } = useCurrentUser();

  return useMemo(() => {
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
}

/**
 * 单权限检查 Hook（保持向后兼容）
 */
export function usePermission(code?: string): boolean {
  const { isSuperAdmin, codesSet } = usePermissionState();

  if (!code) return true;
  if (isSuperAdmin) return true;
  return codesSet.has(code);
}

/**
 * 批量权限检查 Hook（同一组件需要检查多个权限时使用，仅构建一次 Set）
 *
 * @example
 * const { hasPermission } = usePermissions();
 * const canEdit = hasPermission("device:edit");
 * const canDelete = hasPermission("device:delete");
 */
export function usePermissions() {
  const { isSuperAdmin, codesSet } = usePermissionState();

  const hasPermission = useCallback(
    (code: string) => {
      if (!code) return true;
      if (isSuperAdmin) return true;
      return codesSet.has(code);
    },
    [isSuperAdmin, codesSet]
  );

  return { hasPermission, isSuperAdmin };
}
