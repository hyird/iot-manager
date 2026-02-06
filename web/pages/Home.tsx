import {
  ApartmentOutlined,
  ClearOutlined,
  ClockCircleOutlined,
  CloudServerOutlined,
  DesktopOutlined,
  MenuOutlined,
  SettingOutlined,
  TeamOutlined,
  UserOutlined,
} from "@ant-design/icons";
import {
  Button,
  Card,
  Col,
  Divider,
  message,
  Row,
  Skeleton,
  Space,
  Statistic,
  Typography,
} from "antd";
import { useMemo } from "react";
import { useNavigate } from "react-router-dom";
import DynamicIcon from "@/components/DynamicIcon";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import { useClearCache, useHomeStats, useSystemInfo } from "@/services";
import { useAuthStore } from "@/store/hooks";

const { Text, Title } = Typography;

// 快捷入口颜色列表（循环使用）
const QUICK_LINK_COLORS = ["#1677ff", "#52c41a", "#faad14", "#eb2f96", "#722ed1", "#13c2c2"];

function formatUptime(seconds: number): string {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  if (days > 0) {
    return `${days}天 ${hours}小时`;
  }
  if (hours > 0) {
    return `${hours}小时 ${minutes}分钟`;
  }
  return `${minutes}分钟`;
}

export default function HomePage() {
  const { user } = useAuthStore();
  const navigate = useNavigate();

  // ========== 使用 Service Hooks ==========
  const { data: stats, isLoading: statsLoading } = useHomeStats();
  const { data: systemInfo, isLoading: systemLoading } = useSystemInfo();
  const clearCacheMutation = useClearCache();
  const canClearCache = usePermission("system:cache:clear");

  const handleClearCache = () => {
    clearCacheMutation.mutate(undefined, {
      onSuccess: () => {
        message.success("缓存清理成功");
      },
      onError: () => {
        message.error("缓存清理失败");
      },
    });
  };

  // 从用户菜单动态生成快捷入口（只显示 page 类型的菜单）
  const menus = user?.menus;
  const quickLinks = useMemo(() => {
    if (!menus) return [];
    return menus
      .filter((menu) => menu.type === "page" && menu.path)
      .slice(0, 6) // 最多显示6个
      .map((menu, index) => ({
        title: menu.name,
        path: menu.path!,
        icon: menu.icon,
        color: QUICK_LINK_COLORS[index % QUICK_LINK_COLORS.length],
      }));
  }, [menus]);

  // 缓存统计数据，避免每次渲染重新创建
  const statisticsData = useMemo(
    () => [
      {
        title: "用户总数",
        value: stats?.userCount ?? 0,
        icon: <UserOutlined />,
        color: "#1677ff",
      },
      {
        title: "角色数量",
        value: stats?.roleCount ?? 0,
        icon: <TeamOutlined />,
        color: "#52c41a",
      },
      {
        title: "菜单数量",
        value: stats?.menuCount ?? 0,
        icon: <MenuOutlined />,
        color: "#faad14",
      },
      {
        title: "部门数量",
        value: stats?.departmentCount ?? 0,
        icon: <ApartmentOutlined />,
        color: "#eb2f96",
      },
    ],
    [stats]
  );

  return (
    <PageContainer>
      <Title level={4} className="!mb-6">
        欢迎回来，{user?.nickname || user?.username || "用户"}
      </Title>

      {/* 统计卡片 */}
      <Row gutter={[16, 16]}>
        {statisticsData.map((item) => (
          <Col xs={24} sm={12} lg={6} key={item.title}>
            <Card hoverable className="h-full">
              {statsLoading ? (
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

      <Row gutter={[16, 16]} className="mt-4">
        {/* 快捷入口 */}
        <Col xs={24} lg={12}>
          <Card
            title={
              <Space>
                <SettingOutlined />
                <span>快捷入口</span>
              </Space>
            }
            className="h-full"
          >
            <Row gutter={[16, 16]}>
              {quickLinks.map((link) => (
                <Col span={12} key={link.path}>
                  <Card
                    hoverable
                    className="text-center cursor-pointer"
                    onClick={() => navigate(link.path)}
                    styles={{ body: { padding: 16 } }}
                  >
                    <div className="text-2xl mb-2" style={{ color: link.color }}>
                      <DynamicIcon name={link.icon} />
                    </div>
                    <Text>{link.title}</Text>
                  </Card>
                </Col>
              ))}
            </Row>
          </Card>
        </Col>

        {/* 系统信息 */}
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
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <DesktopOutlined className="text-blue-500" />
                    <Text type="secondary">系统版本</Text>
                  </Space>
                  <Text strong>{systemInfo?.version || "1.0.0"}</Text>
                </div>
                <Divider className="!my-2" />
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <ClockCircleOutlined className="text-green-500" />
                    <Text type="secondary">运行时间</Text>
                  </Space>
                  <Text strong>{systemInfo?.uptime ? formatUptime(systemInfo.uptime) : "-"}</Text>
                </div>
                <Divider className="!my-2" />
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <CloudServerOutlined className="text-purple-500" />
                    <Text type="secondary">服务器时间</Text>
                  </Space>
                  <Text strong>
                    {systemInfo?.serverTime
                      ? new Date(systemInfo.serverTime).toLocaleString("zh-CN")
                      : "-"}
                  </Text>
                </div>
                <Divider className="!my-2" />
                <div className="flex items-center justify-between py-2">
                  <Space>
                    <SettingOutlined className="text-orange-500" />
                    <Text type="secondary">运行平台</Text>
                  </Space>
                  <Text strong>{systemInfo?.platform || "-"}</Text>
                </div>
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
