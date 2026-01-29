/**
 * 首页类型定义
 */

/** 统计数据 */
export interface HomeStats {
  userCount: number;
  roleCount: number;
  menuCount: number;
  departmentCount: number;
}

/** 系统信息 */
export interface SystemInfo {
  version: string;
  serverTime: string;
  uptime: number;
  nodeVersion?: string;
  platform?: string;
}
