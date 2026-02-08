/**
 * 菜单管理 QueryKeys
 */

import type { Menu } from "@/types";
import { createQueryKeys } from "../common";

export const menuKeys = createQueryKeys("menus");

/** 扩展的菜单 keys */
export const menuQueryKeys = {
  ...menuKeys,
  /** 菜单列表（带查询参数） */
  list: (params?: Menu.Query) => [...menuKeys.lists(), params] as const,
  /** 菜单树（带状态筛选） */
  tree: (status?: Menu.Status) => [...menuKeys.trees(), { status }] as const,
};
