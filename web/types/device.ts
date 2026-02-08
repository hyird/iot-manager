/**
 * 设备管理类型定义
 */

import type { Link, Protocol } from "@/types";
import type { PageParams } from "./common";

/** 设备状态 */
export type DeviceStatus = "enabled" | "disabled";

/** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
export type ModbusMode = "TCP" | "RTU";

/** 心跳包模式 */
export type HeartbeatMode = "OFF" | "HEX" | "ASCII";

/** 注册包模式 */
export type RegistrationMode = "OFF" | "HEX" | "ASCII";

/** 心跳包配置 */
export interface HeartbeatConfig {
  mode: HeartbeatMode;
  content?: string;
}

/** 注册包配置 */
export interface RegistrationConfig {
  mode: RegistrationMode;
  content?: string;
}

/** 设备列表项 */
export interface DeviceItem {
  id: number;
  name: string;
  /** 设备编码（仅 SL651 设备有值） */
  device_code?: string;
  /** 关联链路 ID */
  link_id: number;
  /** 关联协议配置 ID */
  protocol_config_id: number;
  /** 启用状态 */
  status: DeviceStatus;
  /** 所属分组 ID */
  group_id?: number | null;
  /** 在线超时时间（秒），默认 300 秒（5分钟） */
  online_timeout?: number;
  /** 是否允许远控（下发指令），默认 true */
  remote_control?: boolean;
  /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
  modbus_mode?: ModbusMode;
  /** Modbus 从站地址（1-247），默认 1 */
  slave_id?: number;
  /** 设备时区（用于报文时间解析，默认 +08:00） */
  timezone?: string;
  /** 心跳包配置 */
  heartbeat?: HeartbeatConfig;
  /** 注册包配置 */
  registration?: RegistrationConfig;
  /** 备注 */
  remark?: string;
  created_at?: string;
  updated_at?: string;

  // 关联数据（列表查询时返回）
  link_name?: string;
  link_mode?: Link.Mode;
  link_protocol?: Link.Protocol;
  protocol_name?: string;
  protocol_type?: Protocol.Type;
}

/** 设备选项（下拉列表） */
export interface DeviceOption {
  id: number;
  name: string;
  device_code: string;
}

/** 设备查询参数 */
export interface DeviceQuery extends PageParams {
  link_id?: number;
  protocol_config_id?: number;
  status?: DeviceStatus;
}

/** 创建设备 DTO */
export interface CreateDeviceDto {
  name: string;
  device_code: string;
  link_id: number;
  protocol_config_id: number;
  group_id?: number | null;
  status?: DeviceStatus;
  /** 在线超时时间（秒），默认 300 秒（5分钟） */
  online_timeout?: number;
  /** 是否允许远控（下发指令），默认 true */
  remote_control?: boolean;
  /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
  modbus_mode?: ModbusMode;
  /** Modbus 从站地址（1-247），默认 1 */
  slave_id?: number;
  /** 设备时区（默认 +08:00） */
  timezone?: string;
  /** 心跳包配置 */
  heartbeat?: HeartbeatConfig;
  /** 注册包配置 */
  registration?: RegistrationConfig;
  remark?: string;
}

/** 更新设备 DTO */
export interface UpdateDeviceDto {
  name?: string;
  device_code?: string;
  link_id?: number;
  protocol_config_id?: number;
  group_id?: number | null;
  status?: DeviceStatus;
  /** 在线超时时间（秒） */
  online_timeout?: number;
  /** 是否允许远控（下发指令） */
  remote_control?: boolean;
  /** Modbus 通信模式（仅当链路是 TCP Server 且协议是 Modbus 时使用） */
  modbus_mode?: ModbusMode;
  /** Modbus 从站地址（1-247），默认 1 */
  slave_id?: number;
  /** 设备时区 */
  timezone?: string;
  /** 心跳包配置 */
  heartbeat?: HeartbeatConfig;
  /** 注册包配置 */
  registration?: RegistrationConfig;
  remark?: string;
}

// ========== 设备静态数据类型（支持 ETag 缓存）==========

/** 设备静态数据（不包含实时数据，用于 ETag 缓存） */
export interface DeviceStaticData {
  // 基本信息
  id: number;
  name: string;
  /** 设备编码（仅 SL651 设备返回） */
  device_code?: string;
  link_id: number;
  protocol_config_id: number;
  status: DeviceStatus;
  group_id?: number | null;
  online_timeout?: number;
  remote_control?: boolean;
  modbus_mode?: ModbusMode;
  slave_id?: number;
  timezone?: string;
  heartbeat?: HeartbeatConfig;
  registration?: RegistrationConfig;
  remark?: string;
  created_at?: string;

  // 关联信息
  link_name?: string;
  link_mode?: string;
  protocol_name?: string;
  protocol_type?: string;

  // 协议配置（按协议类型有条件返回）
  downFuncs?: DownFunc[];
  imageFuncs?: ImageFunc[];
}

/** 设备实时数据（用于轮询） */
export interface DeviceRealtimeData {
  id: number;
  reportTime?: string;
  lastHeartbeatTime?: string;
  elements?: DeviceElement[];
  image?: {
    funcCode: string;
    data: string;
  };
}

// ========== 实时数据相关类型 ==========

/** 设备要素数据 */
export interface DeviceElement {
  name: string;
  value: string | number | null;
  unit?: string;
  /** 编码类型 */
  encode?: string;
  /** 字典配置（用于映射值显示） */
  dictConfig?: {
    mapType: "VALUE" | "BIT";
    items: Array<{
      key: string;
      label: string;
      value?: string;
      dependsOn?: {
        operator: "AND" | "OR";
        conditions: Array<{
          bitIndex: string;
          bitValue: string;
        }>;
      };
    }>;
  };
}

/** 下行功能码要素预设值 */
export interface DownFuncElementOption {
  label: string;
  value: string;
}

/** 下行功能码要素 */
export interface DownFuncElement {
  elementId: string;
  name: string;
  value: string;
  unit?: string;
  /** 预设值选项（从协议配置继承） */
  options?: DownFuncElementOption[];
  /** Modbus 寄存器类型 */
  registerType?: string;
  /** Modbus 数据类型 */
  dataType?: string;
}

/** 下行功能码 */
export interface DownFunc {
  funcCode: string;
  name: string;
  elements: DownFuncElement[];
}

/** 图片功能码要素 */
export interface ImageFuncElement {
  elementId: string;
  name: string;
  encode: string;
  unit?: string;
}

/** 图片功能码 */
export interface ImageFunc {
  funcCode: string;
  name: string;
  elements: ImageFuncElement[];
  /** 最新图片数据 */
  latestImage?: {
    data: string;
    size?: number;
  };
}

/** 设备实时数据（包含管理字段） = 静态数据 + 实时字段 */
export interface DeviceRealTimeData extends DeviceStaticData {
  lastHeartbeatTime?: string;
  reportTime?: string;
  elements?: DeviceElement[];
  image?: { data: string };
}

/** 指令下发参数 */
export interface CommandPayload {
  deviceCode: string;
  deviceId?: number;
  funcCode: string;
  elements: Array<{ elementId: string; value: string }>;
}

// ========== 历史数据相关类型 ==========

/** 历史数据类型 */
export type HistoryDataType = "ELEMENT" | "IMAGE";

/** 历史设备列表项 */
export interface HistoryDeviceItem {
  code: string;
  name: string;
  typeName: string;
}

/** 历史功能码项 */
export interface HistoryFuncItem {
  code: string;
  funcCode: string;
  funcName: string;
  dataType: HistoryDataType;
  totalRecords?: number;
}

/** 指令状态 */
export type CommandStatus = "PENDING" | "SUCCESS" | "TIMEOUT" | "SEND_FAILED";

/** 要素历史记录 */
export interface ElementRecord {
  reportTime: string | Date;
  elements: DeviceElement[];
  /** 数据方向：UP=上行（设备上报），DOWN=下行（指令下发） */
  direction?: "UP" | "DOWN";
  /** 应答报文 ID（仅下行指令有，关联的应答记录） */
  responseId?: number;
  /** 应答报文解析的要素（仅下行指令有） */
  responseElements?: DeviceElement[];
  /** 发送用户 ID（仅下行指令有） */
  userId?: number;
  /** 发送用户名（仅下行指令有） */
  userName?: string;
  /** 指令状态（仅下行指令有） */
  status?: CommandStatus;
  /** 失败原因（仅下行指令失败时有） */
  failReason?: string;
}

/** 图片历史记录 */
export interface ImageRecord {
  reportTime: string | Date;
  size: number;
  data: string;
}

/** 历史数据查询参数 */
export interface HistoryDataQuery {
  page?: number;
  pageSize?: number;
  keyword?: string;
  /** 设备编码（SL651 设备使用） */
  code?: string;
  /** 设备 ID（Modbus 设备使用，与 code 二选一） */
  deviceId?: number;
  funcCode?: string;
  dataType?: HistoryDataType;
  startTime?: Date;
  endTime?: Date;
}

/** 设备模块命名空间 */
export namespace Device {
  export type Status = DeviceStatus;
  export type ModbusMode = import("./device").ModbusMode;
  export type Item = DeviceItem;
  export type Option = DeviceOption;
  export type Query = DeviceQuery;
  export type CreateDto = CreateDeviceDto;
  export type UpdateDto = UpdateDeviceDto;
  export type HeartbeatConfig = import("./device").HeartbeatConfig;
  export type RegistrationConfig = import("./device").RegistrationConfig;

  // 静态数据（ETag 缓存）
  export type StaticData = DeviceStaticData;
  // 实时数据（轮询）
  export type Realtime = DeviceRealtimeData;

  // 合并后的完整数据
  export type Element = DeviceElement;
  export type RealTimeData = DeviceRealTimeData;
  export type Command = CommandPayload;
  export type DownFunc = import("./device").DownFunc;
  export type DownFuncElement = import("./device").DownFuncElement;
  export type ImageFunc = import("./device").ImageFunc;
  export type ImageFuncElement = import("./device").ImageFuncElement;

  // 历史数据
  export type HistoryDevice = HistoryDeviceItem;
  export type HistoryFunc = HistoryFuncItem;
  export type HistoryElement = ElementRecord;
  export type HistoryImage = ImageRecord;
  export type HistoryQuery = HistoryDataQuery;
  export type CommandStatus = import("./device").CommandStatus;
}
