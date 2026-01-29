/**
 * CRUD 权限检查 Hook
 */

import { usePermission } from "./usePermission";

/**
 * CRUD 权限配置
 */
export interface CrudPermissions {
  /** 查询权限 */
  canQuery: boolean;
  /** 新增权限 */
  canAdd: boolean;
  /** 编辑权限 */
  canEdit: boolean;
  /** 删除权限 */
  canDelete: boolean;
  /** 导出权限 */
  canExport?: boolean;
  /** 导入权限 */
  canImport?: boolean;
}

/**
 * 使用 CRUD 权限
 * @param module 模块名称，例如 'system:user'
 * @param options 可选权限配置
 * @example
 * ```tsx
 * const permissions = useCrudPermissions('system:user');
 * const permissions = useCrudPermissions('system:user', { export: true, import: true });
 *
 * if (!permissions.canQuery) {
 *   return <Result status="403" />;
 * }
 *
 * <Button disabled={!permissions.canAdd}>新增</Button>
 * <Button disabled={!permissions.canEdit}>编辑</Button>
 * <Button disabled={!permissions.canDelete}>删除</Button>
 * <Button disabled={!permissions.canExport}>导出</Button>
 * ```
 */
export function useCrudPermissions(
  module: string,
  options?: { export?: boolean; import?: boolean }
): CrudPermissions {
  const canQuery = usePermission(`${module}:query`);
  const canAdd = usePermission(`${module}:add`);
  const canEdit = usePermission(`${module}:edit`);
  const canDelete = usePermission(`${module}:delete`);

  // 无条件调用 Hook，然后根据 options 决定是否返回
  const exportPermission = usePermission(`${module}:export`);
  const importPermission = usePermission(`${module}:import`);

  const result: CrudPermissions = {
    canQuery,
    canAdd,
    canEdit,
    canDelete,
  };

  // 根据 options 决定是否包含额外权限
  if (options?.export) {
    result.canExport = exportPermission;
  }
  if (options?.import) {
    result.canImport = importPermission;
  }

  return result;
}
