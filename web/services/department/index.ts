/**
 * 部门管理 Service
 */

// API
export * as departmentApi from "./api";

// Keys
export { departmentKeys, departmentQueryKeys } from "./keys";

// Queries
export {
  useDepartmentList,
  useDepartmentTree,
  useDepartmentDetail,
  useDepartmentTreeSelect,
} from "./queries";

// Mutations
export {
  useDepartmentCreate,
  useDepartmentUpdate,
  useDepartmentDelete,
  useDepartmentSave,
} from "./mutations";
