/**
 * 菜单管理 Service
 */

// API
export * as menuApi from "./api";

// Keys
export { menuKeys, menuQueryKeys } from "./keys";
// Mutations
export { useMenuCreate, useMenuDelete, useMenuSave, useMenuUpdate } from "./mutations";
// Queries
export {
  useMenuDetail,
  useMenuList,
  useMenuTree,
  useMenuTreeForPermission,
  useMenuTreeSelect,
} from "./queries";
