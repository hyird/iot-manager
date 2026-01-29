/**
 * 角色管理 QueryKeys
 */

import { createQueryKeys } from "../common";
import type { Role } from "@/types";

export const roleKeys = createQueryKeys("roles");

/** 扩展的角色 keys */
export const roleQueryKeys = {
  ...roleKeys,
  /** 角色列表（带查询参数） */
  list: (params?: Role.Query) => [...roleKeys.lists(), params] as const,
};
