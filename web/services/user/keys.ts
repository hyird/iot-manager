/**
 * 用户管理 QueryKeys
 */

import type { User } from "@/types";
import { createQueryKeys } from "../common";

export const userKeys = createQueryKeys("users");

/** 扩展的用户 keys */
export const userQueryKeys = {
  ...userKeys,
  /** 用户列表（带查询参数） */
  list: (params?: User.Query) => [...userKeys.lists(), params] as const,
};
