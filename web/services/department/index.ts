/**
 * 部门管理 Service
 */

// API
export * as departmentApi from "./api";

// Keys
export { departmentKeys, departmentQueryKeys } from "./keys";
// Mutations
export {
  useDepartmentCreate,
  useDepartmentDelete,
  useDepartmentSave,
  useDepartmentUpdate,
} from "./mutations";
// Queries
export {
  useDepartmentDetail,
  useDepartmentList,
  useDepartmentTree,
  useDepartmentTreeSelect,
} from "./queries";
