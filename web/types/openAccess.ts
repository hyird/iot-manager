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
    code?: string;
  }

  export interface DeviceRef {
    id: number;
    code?: string;
    name: string;
  }

  export interface DataPoint {
    id: string;
    name: string;
    value: string | number | boolean | null;
    unit: string;
    time?: string | null;
  }

  export interface DeviceDataItem {
    device: DeviceRef;
    points: DataPoint[];
  }

  export interface CommandResult {
    accepted: boolean;
    device: DeviceRef;
  }

  export interface AlertItem {
    id: number;
    device: DeviceRef;
    ruleId: number;
    severity: string;
    status: string;
    message: string;
    time: string;
  }

  export interface WebhookDelivery<T = unknown> {
    event: EventType;
    time: string;
    deliveryId: string;
    data: T;
  }

  export interface WebhookImageData {
    device: DeviceRef;
    image: {
      id: string;
      name: string;
      data: string;
      time?: string | null;
    };
  }

  export interface WebhookCommandData {
    accepted?: boolean;
    device: DeviceRef;
    command: {
      key?: string;
      responseCode?: string;
      responseId?: string | number;
      success?: boolean;
      elements?: unknown;
    };
    points?: DataPoint[];
  }

  export type WebhookData = DeviceDataItem | AlertItem | WebhookImageData | WebhookCommandData;

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
