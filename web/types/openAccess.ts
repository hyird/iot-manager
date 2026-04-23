/**
 * 开放接入类型定义
 */

export namespace OpenAccess {
  export type Status = "enabled" | "disabled";

  export type EventType =
    | "device.data.reported"
    | "device.image.reported"
    | "device.command.dispatched"
    | "device.command.responded"
    | "device.alert.triggered"
    | "device.alert.resolved";

  export type LogDirection = "pull" | "push";

  export type LogStatus = "success" | "failed";

  export interface AccessKeyItem {
    id: number;
    name: string;
    accessKeyPrefix: string;
    status: Status;
    allowRealtime: boolean;
    allowHistory: boolean;
    allowCommand: boolean;
    allowAlert: boolean;
    expiresAt?: string | null;
    lastUsedAt?: string | null;
    lastUsedIp?: string | null;
    remark?: string | null;
    createdAt: string;
    updatedAt: string;
    webhookCount: number;
    deviceIds: number[];
  }

  export interface AccessKeyPayload {
    name: string;
    deviceIds: number[];
    status?: Status;
    allowRealtime?: boolean;
    allowHistory?: boolean;
    allowCommand?: boolean;
    allowAlert?: boolean;
    expiresAt?: string | null;
    remark?: string | null;
  }

  export interface AccessKeyCreateResult {
    id: number;
    name: string;
    status: Status;
    allowRealtime: boolean;
    allowHistory: boolean;
    allowCommand: boolean;
    allowAlert: boolean;
    expiresAt?: string | null;
    deviceIds: number[];
    accessKey: string;
    accessKeyPrefix: string;
  }

  export interface DeviceListItem {
    id: number;
    name: string;
    deviceCode?: string | null;
    status: string;
    typeName?: string | null;
    protocolName?: string | null;
    protocolType?: string | null;
    linkId?: number | null;
    linkName?: string | null;
    groupId?: number | null;
    remoteControl: boolean;
    onlineTimeout: number;
    timezone?: string | null;
    remark?: string | null;
    createdAt?: string | null;
    commandReady: boolean;
    permissions: {
      allowRealtime: boolean;
      allowHistory: boolean;
      allowCommand: boolean;
      allowAlert: boolean;
    };
  }

  export interface WebhookItem {
    id: number;
    accessKeyId: number;
    accessKeyName: string;
    name: string;
    url: string;
    status: Status;
    timeoutSeconds: number;
    headers: Record<string, string | number | boolean>;
    eventTypes: EventType[];
    deviceIds: number[];
    hasSecret: boolean;
    lastTriggeredAt?: string | null;
    lastSuccessAt?: string | null;
    lastFailureAt?: string | null;
    lastHttpStatus?: number | null;
    lastError?: string | null;
    createdAt: string;
    updatedAt: string;
  }

  export interface WebhookPayload {
    accessKeyId: number;
    name: string;
    url: string;
    status?: Status;
    timeoutSeconds?: number;
    secret?: string | null;
    headers?: Record<string, string>;
    eventTypes?: EventType[];
  }

  export interface WebhookQuery {
    accessKeyId?: number;
  }

  export interface AccessLogItem {
    id: number;
    accessKeyId?: number | null;
    accessKeyName?: string | null;
    webhookId?: number | null;
    webhookName?: string | null;
    direction: LogDirection;
    action: string;
    eventType?: string | null;
    status: LogStatus;
    httpMethod?: string | null;
    target?: string | null;
    requestIp?: string | null;
    httpStatus?: number | null;
    deviceId?: number | null;
    deviceCode?: string | null;
    message?: string | null;
    requestPayload: Record<string, unknown>;
    responsePayload: Record<string, unknown>;
    createdAt: string;
  }

  export interface AccessLogQuery {
    page?: number;
    pageSize?: number;
    accessKeyId?: number;
    webhookId?: number;
    deviceId?: number;
    direction?: LogDirection;
    action?: string;
    status?: LogStatus;
    eventType?: string;
  }
}
