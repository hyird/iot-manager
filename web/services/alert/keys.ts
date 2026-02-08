/**
 * 告警管理 Query Keys
 */

export const alertKeys = {
  all: ["alert"] as const,
  rules: () => [...alertKeys.all, "rules"] as const,
  ruleList: (params?: Record<string, unknown>) => [...alertKeys.rules(), params] as const,
  ruleDetail: (id: number) => [...alertKeys.rules(), "detail", id] as const,
  records: () => [...alertKeys.all, "records"] as const,
  recordList: (params?: Record<string, unknown>) => [...alertKeys.records(), params] as const,
  stats: () => [...alertKeys.all, "stats"] as const,
};
