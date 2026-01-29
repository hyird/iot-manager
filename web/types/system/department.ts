/**
 * 部门管理类型定义
 */

// ============ 枚举/状态类型 ============

export type DepartmentStatus = "enabled" | "disabled";

// ============ 列表项/详情类型 ============

export interface DepartmentItem {
  id: number;
  name: string;
  code?: string;
  parent_id?: number | null;
  sort_order: number;
  leader_id?: number | null;
  status: DepartmentStatus;
}

export interface DepartmentTreeItem extends DepartmentItem {
  children?: DepartmentTreeItem[];
}

// ============ 查询参数 ============

export interface DepartmentQuery {
  keyword?: string;
  status?: DepartmentStatus;
}

// ============ DTO 类型 ============

export interface CreateDepartmentDto {
  name: string;
  code?: string;
  parent_id?: number | null;
  sort_order?: number;
  leader_id?: number | null;
  status?: DepartmentStatus;
}

export interface UpdateDepartmentDto {
  name?: string;
  code?: string;
  parent_id?: number | null;
  sort_order?: number;
  leader_id?: number | null;
  status?: DepartmentStatus;
}
