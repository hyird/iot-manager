/**
 * 链路管理类型定义
 */

import type { PageParams } from "./common";

/** 链路状态 */
export type LinkStatus = "enabled" | "disabled";

/** 链路模式 */
export type LinkMode = "TCP Server" | "TCP Client";

/** 链路协议类型 */
export type LinkProtocol = "SL651" | "Modbus" | "Modbus TCP" | "Modbus RTU";

/** 连接状态 */
export type ConnStatus = "stopped" | "listening" | "connected" | "connecting" | "error";

/** 链路列表项 */
export interface LinkItem {
  id: number;
  name: string;
  mode: LinkMode;
  protocol: LinkProtocol;
  ip: string;
  port: number;
  status: LinkStatus;
  /** 连接状态 */
  conn_status?: ConnStatus;
  /** Server 模式下的客户端数量 */
  client_count?: number;
  /** Server 模式下连接的客户端 IP:Port 列表 */
  clients?: string[];
  created_at?: string;
  updated_at?: string;
}

/** 链路选项（下拉列表） */
export interface LinkOption {
  id: number;
  name: string;
  mode: LinkMode;
  protocol: LinkProtocol;
  ip: string;
  port: number;
}

/** 链路查询参数 */
export interface LinkQuery extends PageParams {
  mode?: LinkMode;
}

/** 创建链路 DTO */
export interface CreateLinkDto {
  name: string;
  mode: LinkMode;
  protocol: LinkProtocol;
  ip: string;
  port: number;
  status?: LinkStatus;
}

/** 更新链路 DTO */
export interface UpdateLinkDto {
  name?: string;
  mode?: LinkMode;
  protocol?: LinkProtocol;
  ip?: string;
  port?: number;
  status?: LinkStatus;
}

/** 链路枚举值 */
export interface LinkEnums {
  modes: LinkMode[];
  protocols: LinkProtocol[];
}

/** 链路模块命名空间 */
export namespace Link {
  export type Status = LinkStatus;
  export type Mode = LinkMode;
  export type Protocol = LinkProtocol;
  export type Item = LinkItem;
  export type Option = LinkOption;
  export type Query = LinkQuery;
  export type CreateDto = CreateLinkDto;
  export type UpdateDto = UpdateLinkDto;
  export type Enums = LinkEnums;
}
