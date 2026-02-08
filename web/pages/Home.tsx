import {
  AlertOutlined,
  ClockCircleOutlined,
  CloudOutlined,
  LinkOutlined,
  SwapOutlined,
  ThunderboltOutlined,
} from "@ant-design/icons";
import { App, Badge, Card, Col, Row, Skeleton, Space, Statistic, Typography } from "antd";
import { useMemo } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import { useWsStatus } from "@/providers/WebSocketProvider";
import { useClearCache, useHomeStats, useMonitorData, useSystemInfo } from "@/services";
import { useAuthStore } from "@/store/hooks";
import {
  AlertCenterCard,
  CapacityCard,
  DataQualityCard,
  DataTrendCard,
  DeviceLinkCard,
  ProtocolCard,
  ServerResourceCard,
  ServiceHealthCard,
  SystemInfoCard,
} from "./home/cards";
import { formatNumber } from "./home/utils";

const { Text, Title } = Typography;

export default function HomePage() {
  const { message } = App.useApp();
  const { user } = useAuthStore();
  const { connected: wsConnected } = useWsStatus();

  const { data: stats, isLoading: statsLoading } = useHomeStats();
  const { data: systemInfo, isLoading: systemLoading } = useSystemInfo();
  const {
    data: monitor,
    isLoading: monitorLoading,
    dataUpdatedAt: monitorUpdatedAt,
    isFetching: monitorFetching,
  } = useMonitorData();
  const clearCacheMutation = useClearCache();
  const canClearCache = usePermission("system:cache:clear");

  const handleClearCache = () => {
    clearCacheMutation.mutate(undefined, {
      onSuccess: () => message.success("缓存清理成功"),
    });
  };

  // ==================== 计算指标 ====================

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
        value: formatNumber(stats?.todayDataCount ?? 0),
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

  const onlineRate = monitor?.device.onlineRate ?? 0;

  const linkActiveRate = monitor?.tcp.totalLinks
    ? Math.round((monitor.tcp.activeLinks / monitor.tcp.totalLinks) * 100)
    : 0;

  const pgConnPercent = monitor?.postgres.maxConnections
    ? Math.round(
        ((monitor.postgres.activeConnections + monitor.postgres.idleConnections) /
          monitor.postgres.maxConnections) *
          100
      )
    : 0;

  // ECharts 配置记忆化
  const trendChartOption = useMemo(() => {
    if (!stats?.dataGrowthTrend?.length) return null;
    return {
      tooltip: {
        trigger: "axis" as const,
        formatter: (params: unknown) => {
          const dataItem = (params as Array<{ name: string; value: number }>)[0];
          return `${dataItem.name}<br/>数据量: ${formatNumber(dataItem.value)}`;
        },
      },
      grid: { left: "3%", right: "4%", bottom: "3%", top: "10%", containLabel: true },
      xAxis: {
        type: "category" as const,
        data: stats.dataGrowthTrend.map((item) => {
          const d = new Date(item.date);
          return `${d.getMonth() + 1}/${d.getDate()}`;
        }),
        axisLabel: { fontSize: 11 },
      },
      yAxis: {
        type: "value" as const,
        axisLabel: {
          fontSize: 11,
          formatter: (value: number) => {
            if (value >= 1000) return `${(value / 1000).toFixed(1)}k`;
            return value.toString();
          },
        },
      },
      series: [
        {
          name: "数据量",
          type: "line" as const,
          smooth: true,
          data: stats.dataGrowthTrend.map((item) => item.count),
          itemStyle: { color: "#1677ff" },
          areaStyle: {
            color: {
              type: "linear" as const,
              x: 0,
              y: 0,
              x2: 0,
              y2: 1,
              colorStops: [
                { offset: 0, color: "rgba(22, 119, 255, 0.3)" },
                { offset: 1, color: "rgba(22, 119, 255, 0.05)" },
              ],
            },
          },
        },
      ],
    };
  }, [stats?.dataGrowthTrend]);

  // ==================== 渲染 ====================

  return (
    <PageContainer>
      <div className="flex items-center justify-between mb-6">
        <Title level={4} className="!mb-0">
          欢迎回来，{user?.nickname || user?.username || "用户"}
        </Title>
        <Space>
          <Badge
            status={wsConnected ? "processing" : "default"}
            text={
              <Text type="secondary" className="text-xs">
                {wsConnected ? "实时连接" : "离线"}
              </Text>
            }
          />
          {monitorFetching && (
            <Text type="secondary" className="text-xs">
              <SwapOutlined className="animate-spin mr-1" />
              刷新中
            </Text>
          )}
          {!monitorFetching && monitorUpdatedAt && (
            <Text type="secondary" className="text-xs">
              <ClockCircleOutlined className="mr-1" />
              {new Date(monitorUpdatedAt).toLocaleTimeString("zh-CN")} 更新
            </Text>
          )}
        </Space>
      </div>

      {/* Row 1: 核心业务指标 */}
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
                  valueStyle={{ color: item.color }}
                />
              )}
            </Card>
          </Col>
        ))}
      </Row>

      {/* Row 2: 业务监控（3列） */}
      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} lg={8}>
          <DeviceLinkCard
            monitor={monitor}
            loading={monitorLoading}
            onlineRate={onlineRate}
            linkActiveRate={linkActiveRate}
          />
        </Col>
        <Col xs={24} lg={8}>
          <AlertCenterCard stats={stats} loading={statsLoading} />
        </Col>
        <Col xs={24} lg={8}>
          <DataQualityCard monitor={monitor} loading={monitorLoading} />
        </Col>
      </Row>

      {/* Row 3: 基础设施（2列） */}
      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} lg={12}>
          <ServiceHealthCard
            monitor={monitor}
            loading={monitorLoading}
            pgConnPercent={pgConnPercent}
          />
        </Col>
        <Col xs={24} lg={12}>
          <ServerResourceCard
            monitor={monitor}
            loading={monitorLoading}
            memPercent={memPercent}
            diskPercent={diskPercent}
          />
        </Col>
      </Row>

      {/* Row 4: 协议层 + 系统信息 */}
      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} lg={12}>
          <ProtocolCard monitor={monitor} loading={monitorLoading} />
        </Col>
        <Col xs={24} lg={12}>
          <SystemInfoCard
            systemInfo={systemInfo}
            loading={systemLoading}
            canClearCache={canClearCache}
            clearCacheLoading={clearCacheMutation.isPending}
            onClearCache={handleClearCache}
          />
        </Col>
      </Row>

      {/* Row 5: 容量预警 + 数据趋势 */}
      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} lg={12}>
          <CapacityCard stats={stats} monitor={monitor} loading={statsLoading} />
        </Col>
        <Col xs={24} lg={12}>
          <DataTrendCard stats={stats} loading={statsLoading} trendChartOption={trendChartOption} />
        </Col>
      </Row>
    </PageContainer>
  );
}
