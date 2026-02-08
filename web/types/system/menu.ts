/**
 * 菜单管理类型定义
 */

// ============ 枚举/状态类型 ============

export type MenuType = "menu" | "page" | "button";
export type MenuStatus = "enabled" | "disabled";

// ============ 列表项/详情类型 ============

export interface MenuItem {
  id: number;
  name: string;
  path?: string | null;
  component?: string;
  icon?: string;
  parent_id?: number | null;
  sort_order: number;
  type: MenuType;
  status: MenuStatus;
  permission_code?: string;
}

export interface MenuTreeItem extends MenuItem {
  children?: MenuTreeItem[];
  full_path?: string;
}

// ============ 查询参数 ============

export interface MenuQuery {
  keyword?: string;
  status?: MenuStatus;
}

// ============ DTO 类型 ============

export interface CreateMenuDto {
  name: string;
  path?: string;
  component?: string;
  icon?: string;
  parent_id?: number | null;
  sort_order?: number;
  type?: MenuType;
  status?: MenuStatus;
  permission_code?: string;
}

export interface UpdateMenuDto {
  name?: string;
  path?: string;
  component?: string;
  icon?: string;
  parent_id?: number | null;
  sort_order?: number;
  type?: MenuType;
  status?: MenuStatus;
  permission_code?: string;
}
