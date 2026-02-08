/**
 * 部门管理 QueryKeys
 */

import type { Department } from "@/types";
import { createQueryKeys } from "../common";

export const departmentKeys = createQueryKeys("departments");

/** 扩展的部门 keys */
export const departmentQueryKeys = {
  ...departmentKeys,
  /** 部门列表（带查询参数） */
  list: (params?: Department.Query) => [...departmentKeys.lists(), params] as const,
  /** 部门树（带状态筛选） */
  tree: (status?: Department.Status) => [...departmentKeys.trees(), { status }] as const,
};
