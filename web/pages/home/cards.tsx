/**
 * 首页 memo 子组件
 *
 * 每个 Card 独立 memo 包装，只在自身 props 变化时重渲染。
 * 数据源分离：monitor / stats / systemInfo 各自变化不会连锁触发其他 Card。
 */

import {
  AlertOutlined,
  ApiOutlined,
  CheckCircleOutlined,
  ClearOutlined,
  ClockCircleOutlined,
  CloudOutlined,
  CloudServerOutlined,
  DashboardOutlined,
  DatabaseOutlined,
  DesktopOutlined,
  DisconnectOutlined,
  HddOutlined,
  SafetyOutlined,
  SendOutlined,
  SettingOutlined,
  SwapOutlined,
  ThunderboltOutlined,
  WarningOutlined,
} from "@ant-design/icons";
import {
  Badge,
  Button,
  Card,
  Collapse,
  Divider,
  Progress,
  Skeleton,
  Space,
  Statistic,
  Tag,
  Typography,
} from "antd";
import { LineChart } from "echarts/charts";
import { GridComponent, TooltipComponent } from "echarts/components";
import * as echarts from "echarts/core";
import { SVGRenderer } from "echarts/renderers";
import ReactEChartsCore from "echarts-for-react/lib/core";
import { memo, type ReactNode } from "react";

echarts.use([LineChart, GridComponent, TooltipComponent, SVGRenderer]);

import type { HomeStats, MonitorData, SystemInfo } from "@/services/home/types";
import {
  formatBytes,
  formatDisk,
  formatMemory,
  formatNumber,
  formatUptime,
  getStatusColor,
} from "./utils";

const { Text } = Typography;

// ==================== 共享组件 ====================

export function InfoRow({
  label,
  value,
  icon,
}: {
  label: string;
  value: ReactNode;
  icon?: ReactNode;
}) {
  return (
    <div className="flex items-center justify-between py-2">
      <Space>
        {icon}
        <Text type="secondary">{label}</Text>
      </Space>
      <Text strong>{value}</Text>
    </div>
  );
}

// ==================== Row 2: 设备 & 链路 ====================

export const DeviceLinkCard = memo(function DeviceLinkCard({
  monitor,
  loading,
  onlineRate,
  linkActiveRate,
}: {
  monitor: MonitorData | undefined;
  loading: boolean;
  onlineRate: number;
  linkActiveRate: number;
}) {
  return (
    <Card
      title={
        <Space>
          <CloudOutlined />
          <span>设备 & 链路</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 8 }} />
      ) : (
        <div className="space-y-3">
          <div>
            <div className="flex items-center justify-between mb-2">
              <Text type="secondary">设备在线率</Text>
              <Text strong style={{ color: getStatusColor(onlineRate, true) }}>
                {onlineRate.toFixed(1)}%
              </Text>
            </div>
            <Progress
              percent={Math.round(onlineRate)}
              strokeColor={getStatusColor(onlineRate, true)}
              size="small"
            />
          </div>

          <InfoRow
            label="在线设备"
            icon={<CheckCircleOutlined className="text-green-500" />}
            value={`${monitor?.device.onlineDevices ?? 0} / ${monitor?.device.totalDevices ?? 0}`}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="离线设备"
            icon={<DisconnectOutlined className="text-red-500" />}
            value={monitor?.device.offlineDevices ?? 0}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="数据超时"
            icon={<WarningOutlined className="text-orange-500" />}
            value={monitor?.device.timeoutDevices ?? 0}
          />
          <Divider className="!my-2" />

          <div>
            <div className="flex items-center justify-between mb-2">
              <Text type="secondary">链路活跃率</Text>
              <Text strong>{linkActiveRate}%</Text>
            </div>
            <Progress percent={linkActiveRate} status="active" strokeColor="#1677ff" size="small" />
          </div>

          <InfoRow label="TCP 总连接" value={monitor?.tcp.totalConnections ?? 0} />
          <Divider className="!my-2" />
          <InfoRow
            label="接收流量"
            icon={<SwapOutlined className="text-blue-500" />}
            value={`${formatBytes(monitor?.tcp.bytesRx ?? 0)} / ${formatNumber(monitor?.tcp.packetsRx ?? 0)} 包`}
          />
        </div>
      )}
    </Card>
  );
});

// ==================== Row 2: 告警中心 ====================

export const AlertCenterCard = memo(function AlertCenterCard({
  stats,
  loading,
}: {
  stats: HomeStats | undefined;
  loading: boolean;
}) {
  return (
    <Card
      title={
        <Space>
          <AlertOutlined />
          <span>告警中心</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 8 }} />
      ) : (
        <div className="space-y-3">
          <Statistic
            title="活跃告警"
            value={stats?.activeAlertCount ?? 0}
            valueStyle={{ color: "#ff4d4f" }}
            prefix={<AlertOutlined />}
          />
          <Divider className="!my-2" />

          <div className="space-y-2">
            <Text type="secondary" className="text-xs">
              告警级别分布
            </Text>
            <Space direction="vertical" className="w-full">
              <div className="flex items-center justify-between">
                <Tag color="red" icon={<WarningOutlined />}>
                  严重
                </Tag>
                <Text strong>{stats?.criticalAlertCount ?? 0}</Text>
              </div>
              <div className="flex items-center justify-between">
                <Tag color="orange" icon={<AlertOutlined />}>
                  警告
                </Tag>
                <Text strong>{stats?.warningAlertCount ?? 0}</Text>
              </div>
              <div className="flex items-center justify-between">
                <Tag color="blue" icon={<SafetyOutlined />}>
                  信息
                </Tag>
                <Text strong>{stats?.infoAlertCount ?? 0}</Text>
              </div>
            </Space>
          </div>

          <Divider className="!my-2" />

          <div className="space-y-2">
            <Text type="secondary" className="text-xs">
              今日告警
            </Text>
            <InfoRow
              label="新增告警"
              value={
                <Text
                  strong
                  style={{
                    color: (stats?.todayNewAlertCount ?? 0) > 0 ? "#ff4d4f" : undefined,
                  }}
                >
                  {stats?.todayNewAlertCount ?? 0}
                </Text>
              }
            />
            <InfoRow
              label="已处理"
              value={
                <Text strong style={{ color: "#52c41a" }}>
                  {stats?.todayResolvedAlertCount ?? 0}
                </Text>
              }
            />
          </div>

          {stats?.topAlertDevices && stats.topAlertDevices.length > 0 && (
            <>
              <Divider className="!my-2" />
              <div className="space-y-2">
                <Text type="secondary" className="text-xs">
                  告警热点设备（近7天）
                </Text>
                {stats.topAlertDevices.map((item, idx) => (
                  <div key={idx} className="flex items-center justify-between py-1">
                    <Space size="small">
                      <Badge color="red" />
                      <Text className="text-xs truncate max-w-[120px]">{item.deviceName}</Text>
                    </Space>
                    <Tag color="red" className="m-0">
                      {item.count} 次
                    </Tag>
                  </div>
                ))}
              </div>
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 2: 数据质量 ====================

export const DataQualityCard = memo(function DataQualityCard({
  monitor,
  loading,
}: {
  monitor: MonitorData | undefined;
  loading: boolean;
}) {
  return (
    <Card
      title={
        <Space>
          <DashboardOutlined />
          <span>数据质量</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 8 }} />
      ) : (
        <div className="space-y-3">
          <Statistic
            title="最近1小时数据量"
            value={formatNumber(monitor?.protocol.recentDataCount ?? 0)}
            prefix={<ThunderboltOutlined />}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="采集速率"
            value={`${monitor?.protocol.dataRatePerMin?.toFixed(1) ?? 0} 条/分钟`}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="批量回退率"
            value={
              <Text
                strong
                style={{
                  color: (monitor?.protocol.batchFallbackRate ?? 0) > 5 ? "#faad14" : undefined,
                }}
              >
                {monitor?.protocol.batchFallbackRate?.toFixed(2) ?? 0}%
              </Text>
            }
          />
          <Divider className="!my-2" />
          <InfoRow label="待应答指令" value={monitor?.protocol.pendingCommands ?? 0} />

          {monitor?.sl651 && (
            <>
              <Divider className="!my-2" />
              <Text type="secondary" className="text-xs">
                SL651 协议
              </Text>
              <InfoRow label="解析帧数" value={formatNumber(monitor.sl651.framesParsed)} />
              <InfoRow
                label="错误率"
                value={
                  <Text
                    strong
                    style={{
                      color: monitor.sl651.errorRate > 1 ? "#ff4d4f" : "#52c41a",
                    }}
                  >
                    {monitor.sl651.errorRate.toFixed(2)}%
                  </Text>
                }
              />
            </>
          )}

          {monitor?.modbus && (
            <>
              <Divider className="!my-2" />
              <Text type="secondary" className="text-xs">
                Modbus 协议
              </Text>
              <InfoRow label="响应总数" value={formatNumber(monitor.modbus.totalResponses)} />
              <InfoRow
                label="错误率"
                value={
                  <Text
                    strong
                    style={{
                      color: monitor.modbus.errorRate > 1 ? "#ff4d4f" : "#52c41a",
                    }}
                  >
                    {monitor.modbus.errorRate.toFixed(2)}%
                  </Text>
                }
              />
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 3: 服务健康 ====================

export const ServiceHealthCard = memo(function ServiceHealthCard({
  monitor,
  loading,
  pgConnPercent,
}: {
  monitor: MonitorData | undefined;
  loading: boolean;
  pgConnPercent: number;
}) {
  return (
    <Card
      title={
        <Space>
          <DatabaseOutlined />
          <span>服务健康</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 6 }} />
      ) : (
        <div className="space-y-3">
          <div className="p-3 bg-gray-50 dark:bg-gray-800 rounded">
            <div className="flex items-center justify-between mb-2">
              <Space>
                <Badge status={monitor?.redis.status === "ok" ? "success" : "error"} />
                <Text strong>Redis</Text>
              </Space>
              <Space>
                <Tag color="blue">{monitor?.redis.hitRate?.toFixed(1) ?? 0}% 命中</Tag>
                <Tag color="green">{monitor?.redis.opsPerSec ?? 0} ops/s</Tag>
              </Space>
            </div>
            <InfoRow
              label="内存使用"
              value={`${monitor?.redis.usedMemory ?? "N/A"} / ${monitor?.redis.keyCount ?? 0} keys`}
            />
            <InfoRow label="连接数" value={monitor?.redis.connectedClients ?? 0} />
            {monitor?.redis.uptimeSeconds != null && (
              <InfoRow label="运行时间" value={formatUptime(monitor.redis.uptimeSeconds)} />
            )}
          </div>

          <div className="p-3 bg-gray-50 dark:bg-gray-800 rounded">
            <div className="flex items-center justify-between mb-2">
              <Space>
                <Badge status={monitor?.postgres.status === "ok" ? "success" : "error"} />
                <Text strong>PostgreSQL</Text>
              </Space>
              <Tag color="blue">{monitor?.postgres.cacheHitRatio?.toFixed(1) ?? 0}% 命中</Tag>
            </div>
            <div className="mb-2">
              <div className="flex items-center justify-between mb-1">
                <Text type="secondary" className="text-xs">
                  连接池使用率
                </Text>
                <Text strong>{pgConnPercent}%</Text>
              </div>
              <Progress
                percent={pgConnPercent}
                strokeColor={pgConnPercent > 80 ? "#ff4d4f" : "#1677ff"}
                size="small"
                showInfo={false}
              />
            </div>
            <InfoRow
              label="活跃/空闲"
              value={`${monitor?.postgres.activeConnections ?? 0} / ${monitor?.postgres.idleConnections ?? 0}`}
            />
            <InfoRow label="数据库大小" value={monitor?.postgres.databaseSize ?? "-"} />
          </div>
        </div>
      )}
    </Card>
  );
});

// ==================== Row 3: 服务器资源 ====================

export const ServerResourceCard = memo(function ServerResourceCard({
  monitor,
  loading,
  memPercent,
  diskPercent,
}: {
  monitor: MonitorData | undefined;
  loading: boolean;
  memPercent: number;
  diskPercent: number;
}) {
  return (
    <Card
      title={
        <Space>
          <HddOutlined />
          <span>服务器资源</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 6 }} />
      ) : (
        <div className="space-y-3">
          <div>
            <div className="flex items-center justify-between mb-2">
              <Text type="secondary">系统内存</Text>
              <Text strong>
                {formatMemory(monitor?.server.memoryUsed ?? 0)} /{" "}
                {formatMemory(monitor?.server.memoryTotal ?? 0)}
              </Text>
            </div>
            <Progress percent={memPercent} strokeColor={getStatusColor(memPercent)} size="small" />
          </div>

          <Divider className="!my-2" />

          <div>
            <div className="flex items-center justify-between mb-2">
              <Text type="secondary">磁盘空间</Text>
              <Text strong>
                {formatDisk(monitor?.server.diskUsed ?? 0)} /{" "}
                {formatDisk(monitor?.server.diskTotal ?? 0)}
              </Text>
            </div>
            <Progress
              percent={diskPercent}
              strokeColor={getStatusColor(diskPercent)}
              size="small"
            />
          </div>

          <Divider className="!my-2" />

          <InfoRow
            label="进程内存"
            icon={<HddOutlined className="text-cyan-500" />}
            value={formatMemory(monitor?.server.processMemory ?? 0)}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="主机名"
            icon={<CloudServerOutlined className="text-blue-500" />}
            value={monitor?.server.hostname ?? "-"}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="操作系统"
            icon={<DesktopOutlined className="text-gray-500" />}
            value={monitor?.server.os ?? "-"}
          />
          <Divider className="!my-2" />
          <InfoRow label="CPU 核心" value={`${monitor?.server.cpuCores ?? 0} 核`} />
          {monitor?.server.loadAvg != null && (
            <>
              <Divider className="!my-2" />
              <InfoRow label="系统负载" value={monitor.server.loadAvg.toFixed(2)} />
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 4: 协议处理 ====================

export const ProtocolCard = memo(function ProtocolCard({
  monitor,
  loading,
}: {
  monitor: MonitorData | undefined;
  loading: boolean;
}) {
  return (
    <Card
      title={
        <Space>
          <ApiOutlined />
          <span>协议处理</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 3 }} />
      ) : (
        <div className="space-y-3">
          <div className="grid grid-cols-2 gap-4">
            <div className="text-center">
              <Text type="secondary" className="text-xs block mb-1">
                已处理帧数
              </Text>
              <Text strong className="text-lg text-blue-500">
                {formatNumber(monitor?.protocol.framesProcessed ?? 0)}
              </Text>
            </div>
            <div className="text-center">
              <Text type="secondary" className="text-xs block mb-1">
                待应答指令
              </Text>
              <Text strong className="text-lg text-orange-500">
                {monitor?.protocol.pendingCommands ?? 0}
              </Text>
            </div>
            <div className="text-center">
              <Text type="secondary" className="text-xs block mb-1">
                批量 Flush
              </Text>
              <Text strong className="text-base">
                {formatNumber(monitor?.protocol.batchFlushes ?? 0)}
              </Text>
            </div>
            <div className="text-center">
              <Text type="secondary" className="text-xs block mb-1">
                批量回退
              </Text>
              <Text
                strong
                className="text-base"
                style={{
                  color: (monitor?.protocol.batchFallbacks ?? 0) > 0 ? "#ff4d4f" : undefined,
                }}
              >
                {monitor?.protocol.batchFallbacks ?? 0}
              </Text>
            </div>
          </div>

          {(monitor?.sl651 || monitor?.modbus) && (
            <>
              <Divider className="!my-3" />
              <Collapse
                ghost
                size="small"
                defaultActiveKey={["protocol-details"]}
                items={[
                  {
                    key: "protocol-details",
                    label: (
                      <Text type="secondary" className="text-xs">
                        协议详细统计
                      </Text>
                    ),
                    children: (
                      <div className="space-y-2">
                        {monitor?.sl651 && (
                          <>
                            <Text type="secondary" className="text-xs block">
                              SL651 协议
                            </Text>
                            <div className="grid grid-cols-3 gap-2 text-center">
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  解析帧数
                                </Text>
                                <Text className="text-sm">
                                  {formatNumber(monitor.sl651.framesParsed)}
                                </Text>
                              </div>
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  CRC 错误
                                </Text>
                                <Text
                                  className="text-sm"
                                  style={{
                                    color: monitor.sl651.crcErrors > 0 ? "#ff4d4f" : undefined,
                                  }}
                                >
                                  {monitor.sl651.crcErrors}
                                </Text>
                              </div>
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  多包完成
                                </Text>
                                <Text className="text-sm">
                                  {formatNumber(monitor.sl651.multiPacketCompleted)}
                                </Text>
                              </div>
                            </div>
                          </>
                        )}

                        {monitor?.modbus && (
                          <>
                            <Divider className="!my-2" />
                            <Text type="secondary" className="text-xs block">
                              Modbus 协议
                            </Text>
                            <div className="grid grid-cols-3 gap-2 text-center">
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  响应总数
                                </Text>
                                <Text className="text-sm">
                                  {formatNumber(monitor.modbus.totalResponses)}
                                </Text>
                              </div>
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  平均延迟
                                </Text>
                                <Text className="text-sm">
                                  {monitor.modbus.avgLatencyMs.toFixed(1)} ms
                                </Text>
                              </div>
                              <div>
                                <Text type="secondary" className="text-xs block">
                                  超时
                                </Text>
                                <Text
                                  className="text-sm"
                                  style={{
                                    color: monitor.modbus.timeouts > 0 ? "#faad14" : undefined,
                                  }}
                                >
                                  {monitor.modbus.timeouts}
                                </Text>
                              </div>
                            </div>
                          </>
                        )}
                      </div>
                    ),
                  },
                ]}
              />
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 4: 系统信息 ====================

export const SystemInfoCard = memo(function SystemInfoCard({
  systemInfo,
  loading,
  canClearCache,
  clearCacheLoading,
  onClearCache,
}: {
  systemInfo: SystemInfo | undefined;
  loading: boolean;
  canClearCache: boolean;
  clearCacheLoading: boolean;
  onClearCache: () => void;
}) {
  return (
    <Card
      title={
        <Space>
          <CloudServerOutlined />
          <span>系统信息</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 4 }} />
      ) : (
        <div className="space-y-3">
          <InfoRow
            label="系统版本"
            icon={<DesktopOutlined className="text-blue-500" />}
            value={systemInfo?.version || "1.0.0"}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="运行时间"
            icon={<ClockCircleOutlined className="text-green-500" />}
            value={systemInfo?.uptime ? formatUptime(systemInfo.uptime) : "-"}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="服务器时间"
            icon={<CloudServerOutlined className="text-purple-500" />}
            value={
              systemInfo?.serverTime ? new Date(systemInfo.serverTime).toLocaleString("zh-CN") : "-"
            }
          />
          <Divider className="!my-2" />
          <InfoRow
            label="运行平台"
            icon={<SettingOutlined className="text-orange-500" />}
            value={systemInfo?.platform || "-"}
          />
          {canClearCache && (
            <>
              <Divider className="!my-2" />
              <div className="flex items-center justify-between py-2">
                <Space>
                  <ClearOutlined className="text-red-500" />
                  <Text type="secondary">缓存管理</Text>
                </Space>
                <Button
                  danger
                  size="small"
                  icon={<ClearOutlined />}
                  loading={clearCacheLoading}
                  onClick={onClearCache}
                >
                  清理缓存
                </Button>
              </div>
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 5: 容量预警 ====================

export const CapacityCard = memo(function CapacityCard({
  stats,
  monitor,
  loading,
}: {
  stats: HomeStats | undefined;
  monitor: MonitorData | undefined;
  loading: boolean;
}) {
  return (
    <Card
      title={
        <Space>
          <DatabaseOutlined />
          <span>容量预警</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 4 }} />
      ) : (
        <div className="space-y-3">
          <InfoRow
            label="数据库大小"
            icon={<HddOutlined className="text-blue-500" />}
            value={formatBytes(stats?.databaseSizeBytes ?? 0)}
          />
          <Divider className="!my-2" />
          <InfoRow
            label="预估日增长"
            icon={<SendOutlined className="text-green-500" />}
            value={`${stats?.estimatedDailyGrowthMB?.toFixed(1) ?? 0} MB/天`}
          />

          {monitor && stats && (
            <>
              <Divider className="!my-2" />
              <div className="p-3 bg-blue-50 dark:bg-blue-900/20 rounded">
                <Text type="secondary" className="text-xs">
                  磁盘可用空间：
                  {formatDisk(monitor.server.diskTotal - monitor.server.diskUsed)}
                </Text>
                <br />
                <Text type="secondary" className="text-xs">
                  按当前增速，可存储约{" "}
                  {Math.floor(
                    ((monitor.server.diskTotal - monitor.server.diskUsed) * 1024) /
                      (stats.estimatedDailyGrowthMB || 1)
                  )}{" "}
                  天数据
                </Text>
              </div>
            </>
          )}

          {stats?.topFailureDevices && stats.topFailureDevices.length > 0 && (
            <>
              <Divider className="!my-2" />
              <div className="space-y-2">
                <Text type="secondary" className="text-xs">
                  稳定性较低设备 TOP 5（30天）
                </Text>
                {stats.topFailureDevices.map((item, idx) => (
                  <div key={idx} className="flex items-center justify-between py-1">
                    <Text className="text-xs truncate max-w-[150px]">{item.deviceName}</Text>
                    <Tag color={item.uptimeRate > 80 ? "green" : "orange"}>
                      {item.uptimeRate.toFixed(1)}% 在线
                    </Tag>
                  </div>
                ))}
              </div>
            </>
          )}
        </div>
      )}
    </Card>
  );
});

// ==================== Row 5: 数据趋势 ====================

export const DataTrendCard = memo(function DataTrendCard({
  stats,
  loading,
  trendChartOption,
}: {
  stats: HomeStats | undefined;
  loading: boolean;
  trendChartOption: Record<string, unknown> | null;
}) {
  return (
    <Card
      title={
        <Space>
          <DashboardOutlined />
          <span>数据趋势</span>
        </Space>
      }
      className="h-full"
    >
      {loading ? (
        <Skeleton active paragraph={{ rows: 4 }} />
      ) : (
        <div className="space-y-3">
          {trendChartOption && (
            <div>
              <Text type="secondary" className="text-xs block mb-2">
                近7天数据量趋势
              </Text>
              <ReactEChartsCore
                echarts={echarts}
                option={trendChartOption}
                style={{ height: "180px" }}
                opts={{ renderer: "svg" }}
              />
            </div>
          )}

          {stats?.topAlertRules && stats.topAlertRules.length > 0 && (
            <>
              <Divider className="!my-2" />
              <div className="space-y-2">
                <Text type="secondary" className="text-xs">
                  告警热点规则（近7天）
                </Text>
                {stats.topAlertRules.map((item, idx) => (
                  <div key={idx} className="flex items-center justify-between py-1">
                    <Text className="text-xs truncate max-w-[150px]">{item.ruleName}</Text>
                    <Tag color="red">{item.count} 次</Tag>
                  </div>
                ))}
              </div>
            </>
          )}
        </div>
      )}
    </Card>
  );
});
