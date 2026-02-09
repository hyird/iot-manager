/**
 * 首页类型定义
 */

/** 统计数据 */
export interface HomeStats {
  userCount: number;
  roleCount: number;
  menuCount: number;
  departmentCount: number;
  deviceCount: number;
  linkCount: number;
  todayDataCount: number;
  activeAlertCount: number;
  criticalAlertCount: number;
  warningAlertCount: number;
  infoAlertCount: number;
  todayNewAlertCount: number;
  todayResolvedAlertCount: number;

  // P0: 告警热点分析
  topAlertDevices: Array<{ deviceName: string; count: number }>;
  topAlertRules: Array<{ ruleName: string; count: number }>;

  // P0: 设备健康度评分
  topFailureDevices: Array<{ deviceName: string; onlineDays: number; uptimeRate: number }>;

  // P1: 数据趋势分析
  dataGrowthTrend: Array<{ date: string; count: number }>;

  // P1: 容量预测
  databaseSizeBytes: number;
  estimatedDailyGrowthMB: number;
}

/** 系统信息 */
export interface SystemInfo {
  version: string;
  serverTime: string;
  timezone: string;
  uptime: number;
  platform?: string;
}

/** 监控数据 */
export interface MonitorData {
  tcp: {
    totalLinks: number;
    activeLinks: number;
    totalConnections: number;
    bytesRx: number;
    bytesTx: number;
    packetsRx: number;
    packetsTx: number;
  };
  websocket: {
    connections: number;
    onlineUsers: number;
  };
  redis: {
    status: "ok" | "error";
    usedMemory: string;
    keyCount: number;
    opsPerSec?: number;
    hitRate?: number;
    connectedClients?: number;
    uptimeSeconds?: number;
  };
  postgres: {
    status: "ok" | "error";
    activeConnections: number;
    idleConnections: number;
    maxConnections: number;
    cacheHitRatio?: number;
    xactCommit?: number;
    xactRollback?: number;
    databaseSize?: string;
  };
  device: {
    registeredDevices: number;
    registeredClients: number;
    totalDevices: number;
    onlineDevices: number;
    offlineDevices: number;
    timeoutDevices: number;
    onlineRate: number;
  };
  protocol: {
    pendingCommands: number;
    framesProcessed: number;
    batchFlushes: number;
    batchFallbacks: number;
    recentDataCount: number;
    dataRatePerMin: number;
    batchFallbackRate: number;
  };
  modbus?: {
    totalResponses: number;
    avgLatencyMs: number;
    timeouts: number;
    crcErrors: number;
    exceptions: number;
    errorRate: number;
  };
  sl651?: {
    framesParsed: number;
    crcErrors: number;
    multiPacketCompleted: number;
    multiPacketExpired: number;
    parseErrors: number;
    errorRate: number;
  };
  server: {
    processMemory: number;
    hostname: string;
    os: string;
    cpuCores: number;
    loadAvg?: number;
    memoryTotal: number;
    memoryUsed: number;
    diskTotal: number;
    diskUsed: number;
  };
}
