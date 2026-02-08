/**
 * 告警模块类型定义
 */

export namespace Alert {
  /** 严重级别 */
  export type Severity = "critical" | "warning" | "info";

  /** 告警记录状态 */
  export type RecordStatus = "active" | "acknowledged" | "resolved";

  /** 条件类型 */
  export type ConditionType = "threshold" | "offline" | "rate_of_change";

  /** 比较运算符 */
  export type Operator = ">" | ">=" | "<" | "<=" | "==" | "!=";

  /** 变化方向 */
  export type ChangeDirection = "rise" | "fall" | "any";

  /** 告警条件 */
  export interface Condition {
    type: ConditionType;
    elementKey?: string;
    operator?: Operator;
    value?: string;
    duration?: number;
    changeRate?: string;
    changeDirection?: ChangeDirection;
  }

  /** 告警规则列表项 */
  export interface RuleItem {
    id: number;
    name: string;
    device_id: number;
    device_name: string;
    severity: Severity;
    conditions: Condition[];
    logic: "and" | "or";
    silence_duration: number;
    status: "enabled" | "disabled";
    remark: string;
    created_at: string;
    updated_at: string;
  }

  /** 告警规则创建/更新 DTO */
  export interface RuleDto {
    name: string;
    device_id: number;
    severity: Severity;
    conditions: Condition[];
    logic: "and" | "or";
    silence_duration: number;
    status?: "enabled" | "disabled";
    remark?: string;
  }

  /** 告警记录列表项 */
  export interface RecordItem {
    id: number;
    rule_id: number;
    rule_name: string;
    device_id: number;
    device_name: string;
    severity: Severity;
    status: RecordStatus;
    message: string;
    detail: Record<string, unknown>;
    triggered_at: string;
    acknowledged_at?: string;
    acknowledged_by?: number;
    resolved_at?: string;
  }

  /** 活跃告警统计 */
  export interface ActiveStats {
    total: number;
    critical: number;
    warning: number;
    info: number;
  }

  /** 告警规则模板列表项 */
  export interface TemplateItem {
    id: number;
    name: string;
    category: string;
    description: string;
    severity: Severity;
    logic: "and" | "or";
    silence_duration: number;
    created_at: string;
  }

  /** 告警规则模板详情 */
  export interface TemplateDetail {
    id: number;
    name: string;
    category: string;
    description: string;
    severity: Severity;
    conditions: Condition[];
    logic: "and" | "or";
    silence_duration: number;
    recovery_condition: string;
    recovery_wait_seconds: number;
    applicable_protocols: string[];
    created_by: number;
    created_at: string;
  }

  /** 告警规则模板创建/更新 DTO */
  export interface TemplateDto {
    name: string;
    category?: string;
    description?: string;
    severity: Severity;
    conditions: Condition[];
    logic: "and" | "or";
    silence_duration: number;
    recovery_condition?: string;
    recovery_wait_seconds?: number;
    applicable_protocols?: string[];
  }

  /** 模板批量应用请求 */
  export interface ApplyTemplateRequest {
    template_id: number;
    device_ids: number[];
  }

  /** 模板批量应用响应 */
  export interface ApplyTemplateResponse {
    success: number;
    total: number;
    createdIds: number[];
  }
}
