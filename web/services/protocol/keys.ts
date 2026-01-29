/**
 * 协议配置 QueryKeys
 */

import { createQueryKeys } from "../common";
import type { Protocol } from "@/types";

export const protocolKeys = createQueryKeys("protocol");

/** 扩展的协议配置 keys */
export const protocolQueryKeys = {
  ...protocolKeys,
  /** 配置列表（带查询参数） */
  list: (params?: Protocol.Query) => [...protocolKeys.lists(), params] as const,
  /** 配置选项（按协议类型） */
  options: (protocol: Protocol.Type) => [...protocolKeys.all, "options", protocol] as const,
};
