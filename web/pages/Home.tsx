import {
  AlertOutlined,
  ApiOutlined,
  ClearOutlined,
  ClockCircleOutlined,
  CloudOutlined,
  CloudServerOutlined,
  DashboardOutlined,
  DatabaseOutlined,
  DesktopOutlined,
  HddOutlined,
  LinkOutlined,
  SendOutlined,
  SettingOutlined,
  SwapOutlined,
  ThunderboltOutlined,
  UserOutlined,
  WarningOutlined,
} from "@ant-design/icons";
import {
  App,
  Button,
  Card,
  Col,
  Divider,
  Progress,
  Row,
  Skeleton,
  Space,
  Statistic,
  Tag,
  Typography,
} from "antd";
import { useMemo } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import { useClearCache, useHomeStats, useMonitorData, useSystemInfo } from "@/services";
import { useAuthStore } from "@/store/hooks";

const { Text, Title } = Typography;

function formatUptime(seconds: number): string {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  if (days > 0) return `${days}天 ${hours}小时`;
  if (hours > 0) return `${hours}小时 ${minutes}分钟`;
  return `${minutes}分钟`;
}

function formatMemory(mb: number): string {
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${Math.round(mb)} MB`;
}

function formatDisk(gb: number): string {
  return `${gb.toFixed(1)} GB`;
}

function formatBytes(bytes: number): string {
  if (bytes >= 1073741824) return `${(bytes / 1073741824).toFixed(2)} GB`;
  if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

function formatNumber(n: number): string {
  if (n >= 1000000) return `${(n / 1000000).toFixed(1)}M`;
  if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
  return String(n);
}

/** 信息行组件 */
function InfoRow({
  label,
  value,
  icon,
}: {
  label: string;
  value: React.ReactNode;
  icon?: React.ReactNode;
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

export default function HomePage() {
  const { message } = App.useApp();
  const { user } = useAuthStore();

  const { data: stats, isLoading: statsLoading } = useHomeStats();
  const { data: systemInfo, isLoading: systemLoading } = useSystemInfo();
  const { data: monitor, isLoading: monitorLoading } = useMonitorData();
  const clearCacheMutation = useClearCache();
  const canClearCache = usePermission("system:cache:clear");

  const handleClearCache = () => {
    clearCacheMutation.mutate(undefined, {
      onSuccess: () => message.success("缓存清理成功"),
      onError: () => message.error("缓存清理失败"),
    });
  };

  const coreStatsData = useMemo(
    () => [
      {
        title: "设备总数",
        value: stats?.deviceCount ?? 0,
        icon: <CloudOutlined />,
        color: "#1677ff",
      },
      {
        title: "在线链路",
        value: `${monitor?.tcp.activeLinks ?? 0} / ${monitor?.tcp.totalLinks ?? 0}`,
        icon: <LinkOutlined />,
        color: "#52c41a",
      },
      {
        title: "今日数据量",
        value: stats?.todayDataCount ?? 0,
        icon: <ThunderboltOutlined />,
        color: "#faad14",
      },
      {
        title: "活跃告警",
        value: stats?.activeAlertCount ?? 0,
        icon: <AlertOutlined />,
        color: "#ff4d4f",
      },
    ],
    [stats, monitor]
  );

  const isLoading = statsLoading || monitorLoading;

  const memPercent = monitor?.server.memoryTotal
    ? Math.round((monitor.server.memoryUsed / monitor.server.memoryTotal) * 100)
    : 0;

  const diskPercent = monitor?.server.diskTotal
    ? Math.round((monitor.server.diskUsed / monitor.server.diskTotal) * 100)
    : 0;

  return (
    <PageContainer>
      <Title level={4} className="!mb-6">
        欢迎回来，{user?.nickname || user?.username || "用户"}
      </Title>

      {/* 核心业务指标 */}
      <Row gutter={[16, 16]}>
        {coreStatsData.map((item) => (
          <Col xs={24} sm={12} lg={6} key={item.title}>
            <Card hoverable className="h-full">
              {isLoading ? (
                <Skeleton active paragraph={{ rows: 1 }} />
              ) : (
                <Statistic
                  title={item.title}
                  value={item.value}
                  prefix={item.icon}
                  styles={{ content: { color: item.color } }}
                />
              )}
            </Card>
          </Col>
        ))}
      </Row>

      {/* 监控区域 — 3 列 */}
      <Row gutter={[16, 16]} className="mt-4">
        {/* 连接 & 吞吐 */}
        <Col xs={24} lg={8}>
          <Card
            title={
              <Space>
                <ApiOutlined />
                <span>连接状态</span>
              </Space>
            }
            className="h-full"
          >
            {monitorLoading ? (
              <Skeleton active paragraph={{ rows: 8 }} />
            ) : (
              <div className="space-y-3">
                <InfoRow
                  label="活跃链路"
                  value={`${monitor?.tcp.activeLinks ?? 0} / ${monitor?.tcp.totalLinks ?? 0}`}
                />
                <Progress
                  percent={
                    monitor?.tcp.totalLinks
                      ? Math.round((monitor.tcp.activeLinks / monitor.tcp.totalLinks) * 100)
                      : 0
                  }
                  status="active"
                  size="small"
                />
                <Divider className="!my-2" />
                <InfoRow label="TCP 总连接数" value={monitor?.tcp.totalConnections ?? 0} />
                <Divider className="!my-2" />
                <InfoRow
                  label="接收流量"
                  icon={<SwapOutlined className="text-blue-500" />}
                  value={`${formatBytes(monitor?.tcp.bytesRx ?? 0)} / ${formatNumber(monitor?.tcp.packetsRx ?? 0)} 包`}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="发送流量"
                  icon={<SwapOutlined className="text-green-500" />}
                  value={`${formatBytes(monitor?.tcp.bytesTx ?? 0)} / ${formatNumber(monitor?.tcp.packetsTx ?? 0)} 包`}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="注册设备"
                  icon={<CloudOutlined className="text-blue-500" />}
                  value={monitor?.device.registeredDevices ?? 0}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="注册终端"
                  icon={<DesktopOutlined className="text-green-500" />}
                  value={monitor?.device.registeredClients ?? 0}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="在线用户"
                  icon={<UserOutlined className="text-purple-500" />}
                  value={monitor?.websocket.onlineUsers ?? 0}
                />
              </div>
            )}
          </Card>
        </Col>

        {/* 服务状态 */}
        <Col xs={24} lg={8}>
          <Card
            title={
              <Space>
                <DatabaseOutlined />
                <span>服务状态</span>
              </Space>
            }
            className="h-full"
          >
            {monitorLoading ? (
              <Skeleton active paragraph={{ rows: 8 }} />
            ) : (
              <div className="space-y-3">
                {/* Redis */}
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <Text type="secondary">Redis</Text>
                    <Tag color={monitor?.redis.status === "ok" ? "success" : "error"}>
                      {monitor?.redis.status === "ok" ? "正常" : "异常"}
                    </Tag>
                  </Space>
                  <Text strong>
                    {monitor?.redis.usedMemory ?? "N/A"} / {monitor?.redis.keyCount ?? 0} keys
                  </Text>
                </div>
                {monitor?.redis.hitRate != null && (
                  <>
                    <InfoRow label="缓存命中率" value={`${monitor.redis.hitRate.toFixed(1)}%`} />
                    <Progress
                      percent={Math.round(monitor.redis.hitRate)}
                      strokeColor={
                        monitor.redis.hitRate > 80
                          ? "#52c41a"
                          : monitor.redis.hitRate > 60
                            ? "#faad14"
                            : "#ff4d4f"
                      }
                      size="small"
                    />
                  </>
                )}
                <InfoRow label="Ops/sec" value={monitor?.redis.opsPerSec ?? 0} />
                <InfoRow label="Redis 连接" value={monitor?.redis.connectedClients ?? 0} />
                {monitor?.redis.uptimeSeconds != null && (
                  <InfoRow label="Redis 运行" value={formatUptime(monitor.redis.uptimeSeconds)} />
                )}

                <Divider className="!my-2" />

                {/* PostgreSQL */}
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <Text type="secondary">PostgreSQL</Text>
                    <Tag color={monitor?.postgres.status === "ok" ? "success" : "error"}>
                      {monitor?.postgres.status === "ok" ? "正常" : "异常"}
                    </Tag>
                  </Space>
                  <Text strong>
                    {monitor?.postgres.activeConnections ?? 0} /{" "}
                    {monitor?.postgres.maxConnections ?? 0} 连接
                  </Text>
                </div>
                <Progress
                  percent={
                    monitor?.postgres.maxConnections
                      ? Math.round(
                          ((monitor.postgres.activeConnections + monitor.postgres.idleConnections) /
                            monitor.postgres.maxConnections) *
                            100
                        )
                      : 0
                  }
                  strokeColor={
                    (monitor?.postgres.activeConnections ?? 0) /
                      (monitor?.postgres.maxConnections || 1) >
                    0.8
                      ? "#ff4d4f"
                      : "#1677ff"
                  }
                  size="small"
                />
                <InfoRow label="空闲连接" value={monitor?.postgres.idleConnections ?? 0} />
                {monitor?.postgres.cacheHitRatio != null && (
                  <InfoRow
                    label="缓存命中率"
                    value={`${monitor.postgres.cacheHitRatio.toFixed(1)}%`}
                  />
                )}
                <InfoRow label="数据库大小" value={monitor?.postgres.databaseSize ?? "-"} />
                <InfoRow
                  label="事务"
                  value={`${formatNumber(monitor?.postgres.xactCommit ?? 0)} 提交 / ${formatNumber(monitor?.postgres.xactRollback ?? 0)} 回滚`}
                />
              </div>
            )}
          </Card>
        </Col>

        {/* 服务器资源 */}
        <Col xs={24} lg={8}>
          <Card
            title={
              <Space>
                <HddOutlined />
                <span>服务器资源</span>
              </Space>
            }
            className="h-full"
          >
            {monitorLoading ? (
              <Skeleton active paragraph={{ rows: 8 }} />
            ) : (
              <div className="space-y-3">
                <InfoRow
                  label="系统内存"
                  value={`${formatMemory(monitor?.server.memoryUsed ?? 0)} / ${formatMemory(monitor?.server.memoryTotal ?? 0)}`}
                />
                <Progress
                  percent={memPercent}
                  strokeColor={
                    memPercent > 85 ? "#ff4d4f" : memPercent > 70 ? "#faad14" : "#52c41a"
                  }
                  size="small"
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="磁盘空间"
                  value={`${formatDisk(monitor?.server.diskUsed ?? 0)} / ${formatDisk(monitor?.server.diskTotal ?? 0)}`}
                />
                <Progress
                  percent={diskPercent}
                  strokeColor={
                    diskPercent > 90 ? "#ff4d4f" : diskPercent > 75 ? "#faad14" : "#52c41a"
                  }
                  size="small"
                />
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
        </Col>
      </Row>

      {/* 协议处理 + 系统信息 */}
      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} lg={12}>
          <Card
            title={
              <Space>
                <DashboardOutlined />
                <span>协议处理</span>
              </Space>
            }
            className="h-full"
          >
            {monitorLoading ? (
              <Skeleton active paragraph={{ rows: 5 }} />
            ) : (
              <div className="space-y-3">
                <InfoRow
                  label="已处理帧数"
                  icon={<ThunderboltOutlined className="text-blue-500" />}
                  value={formatNumber(monitor?.protocol.framesProcessed ?? 0)}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="批量 Flush"
                  value={formatNumber(monitor?.protocol.batchFlushes ?? 0)}
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="批量回退"
                  icon={
                    (monitor?.protocol.batchFallbacks ?? 0) > 0 ? (
                      <WarningOutlined className="text-red-500" />
                    ) : undefined
                  }
                  value={
                    <span
                      style={{
                        color: (monitor?.protocol.batchFallbacks ?? 0) > 0 ? "#ff4d4f" : undefined,
                      }}
                    >
                      {monitor?.protocol.batchFallbacks ?? 0}
                    </span>
                  }
                />
                <Divider className="!my-2" />
                <InfoRow
                  label="待应答指令"
                  icon={<SendOutlined className="text-orange-500" />}
                  value={monitor?.protocol.pendingCommands ?? 0}
                />
                {monitor?.sl651 && (
                  <>
                    <Divider className="!my-2" />
                    <Text type="secondary" className="text-xs">
                      SL651 统计
                    </Text>
                    <InfoRow label="解析帧数" value={formatNumber(monitor.sl651.framesParsed)} />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="CRC 错误"
                      value={
                        <span
                          style={{
                            color: monitor.sl651.crcErrors > 0 ? "#ff4d4f" : undefined,
                          }}
                        >
                          {monitor.sl651.crcErrors}
                        </span>
                      }
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="多包完成"
                      value={formatNumber(monitor.sl651.multiPacketCompleted)}
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="多包超时"
                      value={
                        <span
                          style={{
                            color: monitor.sl651.multiPacketExpired > 0 ? "#faad14" : undefined,
                          }}
                        >
                          {monitor.sl651.multiPacketExpired}
                        </span>
                      }
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="解析错误"
                      value={
                        <span
                          style={{
                            color: monitor.sl651.parseErrors > 0 ? "#ff4d4f" : undefined,
                          }}
                        >
                          {monitor.sl651.parseErrors}
                        </span>
                      }
                    />
                  </>
                )}
                {monitor?.modbus && (
                  <>
                    <Divider className="!my-2" />
                    <Text type="secondary" className="text-xs">
                      Modbus 统计
                    </Text>
                    <InfoRow label="响应总数" value={formatNumber(monitor.modbus.totalResponses)} />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="平均延迟"
                      value={`${monitor.modbus.avgLatencyMs.toFixed(1)} ms`}
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="超时"
                      value={
                        <span
                          style={{
                            color: monitor.modbus.timeouts > 0 ? "#faad14" : undefined,
                          }}
                        >
                          {monitor.modbus.timeouts}
                        </span>
                      }
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="CRC 错误"
                      value={
                        <span
                          style={{
                            color: monitor.modbus.crcErrors > 0 ? "#ff4d4f" : undefined,
                          }}
                        >
                          {monitor.modbus.crcErrors}
                        </span>
                      }
                    />
                    <Divider className="!my-2" />
                    <InfoRow
                      label="异常响应"
                      value={
                        <span
                          style={{
                            color: monitor.modbus.exceptions > 0 ? "#faad14" : undefined,
                          }}
                        >
                          {monitor.modbus.exceptions}
                        </span>
                      }
                    />
                  </>
                )}
              </div>
            )}
          </Card>
        </Col>

        <Col xs={24} lg={12}>
          <Card
            title={
              <Space>
                <CloudServerOutlined />
                <span>系统信息</span>
              </Space>
            }
            className="h-full"
          >
            {systemLoading ? (
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
                    systemInfo?.serverTime
                      ? new Date(systemInfo.serverTime).toLocaleString("zh-CN")
                      : "-"
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
                        loading={clearCacheMutation.isPending}
                        onClick={handleClearCache}
                      >
                        清理缓存
                      </Button>
                    </div>
                  </>
                )}
              </div>
            )}
          </Card>
        </Col>
      </Row>
    </PageContainer>
  );
}
