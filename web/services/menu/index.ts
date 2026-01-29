/**
 * 菜单管理 Service
 */

// API
export * as menuApi from "./api";

// Keys
export { menuKeys, menuQueryKeys } from "./keys";

// Queries
export {
  useMenuList,
  useMenuTree,
  useMenuDetail,
  useMenuTreeSelect,
  useMenuTreeForPermission,
} from "./queries";

// Mutations
export { useMenuCreate, useMenuUpdate, useMenuDelete, useMenuSave } from "./mutations";
