/**
 * 系统管理模块类型统一导出
 */

export * from "./auth";
export * from "./user";
export * from "./role";
export * from "./menu";
export * from "./department";

// ============ 命名空间别名 ============

import type {
  UserStatus,
  UserRole,
  UserItem,
  UserQuery,
  CreateUserDto,
  UpdateUserDto,
  UpdatePasswordDto as UserUpdatePasswordDto,
} from "./user";
import type {
  RoleStatus,
  RoleItem,
  RoleDetail,
  RoleOption,
  RoleQuery,
  CreateRoleDto,
  UpdateRoleDto,
} from "./role";
import type {
  MenuType,
  MenuStatus,
  MenuItem,
  MenuTreeItem,
  MenuQuery,
  CreateMenuDto,
  UpdateMenuDto,
} from "./menu";
import type {
  DepartmentStatus,
  DepartmentItem,
  DepartmentTreeItem,
  DepartmentQuery,
  CreateDepartmentDto,
  UpdateDepartmentDto,
} from "./department";

/** 认证模块命名空间 */
export namespace Auth {
  export type JwtPayload = import("./auth").JwtPayload;
  export type LoginRequest = import("./auth").LoginRequest;
  export type LoginResult = import("./auth").LoginResult;
  export type UserInfo = import("./auth").UserInfo;
}

/** 用户模块命名空间 */
export namespace User {
  export type Status = UserStatus;
  export type Role = UserRole;
  export type Item = UserItem;
  export type Query = UserQuery;
  export type CreateDto = CreateUserDto;
  export type UpdateDto = UpdateUserDto;
  export type UpdatePasswordDto = UserUpdatePasswordDto;
}

/** 角色模块命名空间 */
export namespace Role {
  export type Status = RoleStatus;
  export type Item = RoleItem;
  export type Detail = RoleDetail;
  export type Option = RoleOption;
  export type Query = RoleQuery;
  export type CreateDto = CreateRoleDto;
  export type UpdateDto = UpdateRoleDto;
}

/** 菜单模块命名空间 */
export namespace Menu {
  export type Type = MenuType;
  export type Status = MenuStatus;
  export type Item = MenuItem;
  export type TreeItem = MenuTreeItem;
  export type Query = MenuQuery;
  export type CreateDto = CreateMenuDto;
  export type UpdateDto = UpdateMenuDto;
}

/** 部门模块命名空间 */
export namespace Department {
  export type Status = DepartmentStatus;
  export type Item = DepartmentItem;
  export type TreeItem = DepartmentTreeItem;
  export type Query = DepartmentQuery;
  export type CreateDto = CreateDepartmentDto;
  export type UpdateDto = UpdateDepartmentDto;
}
