/**
 * 采集 Agent 类型定义
 */

export interface AgentInterface {
  name: string;
  display_name?: string;
  ip?: string;
  prefix_length?: number;
  up?: boolean;
  /** 系统检测到的地址模式: dhcp / static / unknown */
  method?: "dhcp" | "static" | "unknown";
  /** 当前默认网关 */
  gateway?: string;
  /** 桥接口的成员端口列表（仅桥接口有此字段） */
  bridge_ports?: string[];
}

export type AgentTransport = "ethernet" | "serial";

export interface AgentEndpointDevice {
  device_id: number;
  device_name: string;
  protocol?: string;
}

/** Agent 端点（来自 agent_endpoint 表） */
export interface AgentEndpointItem {
  id: number;
  agent_id: number;
  name: string;
  transport: AgentTransport;
  mode?: string;
  protocol: string;
  ip?: string;
  port?: number;
  channel?: string;
  baud_rate?: number;
  status?: string;
  device_count?: number;
  devices?: AgentEndpointDevice[];
}

export interface AgentEndpointCreateInput {
  name: string;
  transport: AgentTransport;
  mode?: string;
  protocol: string;
  ip?: string;
  port?: number;
  channel?: string;
  baud_rate?: number;
}

export interface AgentEndpointUpdateInput {
  name?: string;
  mode?: string;
  protocol?: string;
  ip?: string;
  port?: number;
  channel?: string;
  baud_rate?: number;
  status?: string;
}

export type AgentEventLevel = "info" | "success" | "warning" | "error";

export interface AgentRecentEvent {
  type: string;
  level?: AgentEventLevel;
  message: string;
  ts?: string;
  detail?: {
    configVersion?: number;
    managedDeviceCount?: number;
    error?: string;
    version?: string;
    code?: string;
  };
}

export interface AgentCapabilities {
  interfaces?: AgentInterface[];
  serial_ports?: AgentSerialPort[];
  supports?: {
    tcp?: boolean;
    serial?: boolean;
    rawRelay?: boolean;
  };
  /** 系统检测到的网络管理后端 (NetworkManager / systemd-networkd / netplan 等) */
  network_backend?: string;
}

export interface AgentSerialPort {
  name: string;
  display_name?: string;
  available?: boolean;
}

export interface AgentRuntime {
  hostname?: string;
  managedDeviceCount?: number;
  arch?: string;
  platform?: string;
  lastAppliedConfigVersion?: number;
}

export interface AgentNetworkConfigItem {
  name: string;
  mode: "dhcp" | "static" | "none";
  type?: "ethernet" | "bridge";
  ip?: string;
  prefix_length?: number;
  gateway?: string;
  /** 桥接口的成员端口列表 */
  bridge_ports?: string[];
  /** 桥接操作: delete = 删除桥接 */
  action?: "delete";
}

export interface AgentItem {
  id: number;
  code: string;
  name: string;
  version?: string;
  is_online: boolean;
  last_seen?: string;
  connected_at?: string;
  expected_config_version?: number;
  applied_config_version?: number;
  config_status?: "idle" | "pending" | "applied" | "failed";
  config_error?: string;
  last_config_sync_at?: string;
  last_config_applied_at?: string;
  managed_device_count?: number;
  interfaces?: AgentInterface[];
  expected_endpoints?: AgentEndpointItem[];
  recent_events?: AgentRecentEvent[];
  last_event_at?: string;
  capabilities?: AgentCapabilities;
  runtime?: AgentRuntime;
  network_config?: AgentNetworkConfigItem[];
}

export interface AgentCreateInput {
  code: string;
  name: string;
}

export interface AgentUpdateInput {
  name?: string;
}

export namespace Agent {
  export type Capabilities = AgentCapabilities;
  export type Event = AgentRecentEvent;
  export type Endpoint = AgentEndpointItem;
  export type EndpointDevice = AgentEndpointDevice;
  export type EndpointCreate = AgentEndpointCreateInput;
  export type EndpointUpdate = AgentEndpointUpdateInput;
  export type Transport = AgentTransport;
  export type Interface = AgentInterface;
  export type Item = AgentItem;
  export type Runtime = AgentRuntime;
  export type NetworkConfigItem = AgentNetworkConfigItem;
  export type CreateInput = AgentCreateInput;
  export type UpdateInput = AgentUpdateInput;
  /** @deprecated use Agent.Item */
  export type Option = AgentItem;
}
