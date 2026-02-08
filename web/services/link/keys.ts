/**
 * 链路管理 QueryKeys
 */

import type { Link } from "@/types";
import { createQueryKeys } from "../common";

export const linkKeys = createQueryKeys("links");

/** 扩展的链路 keys */
export const linkQueryKeys = {
  ...linkKeys,
  /** 链路列表（带查询参数） */
  list: (params?: Link.Query) => [...linkKeys.lists(), params] as const,
  /** 链路选项 */
  options: () => [...linkKeys.all, "options"] as const,
  /** 链路枚举值 */
  enums: () => [...linkKeys.all, "enums"] as const,
  /** 服务器公网 IP */
  publicIp: () => [...linkKeys.all, "publicIp"] as const,
};
