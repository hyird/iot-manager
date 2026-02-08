/**
 * 角色管理 Service
 */

// API
export * as roleApi from "./api";

// Keys
export { roleKeys, roleQueryKeys } from "./keys";
// Mutations
export { useRoleCreate, useRoleDelete, useRoleSave, useRoleUpdate } from "./mutations";
// Queries
export { useRoleDetail, useRoleList, useRoleOptions } from "./queries";
