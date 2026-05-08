import {
  PauseCircleOutlined,
  PlayCircleOutlined,
  ReloadOutlined,
  SendOutlined,
  VideoCameraOutlined,
} from "@ant-design/icons";
import { App, Button, Card, Input, Result, Select, Space, Tag, Typography } from "antd";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import {
  stopPreviewKeepalive,
  useGb28181CatalogQuery,
  useGb28181Devices,
  useGb28181Health,
  useGb28181PreviewStart,
  useGb28181PreviewStop,
  useGb28181Ptz,
} from "@/services";
import { useAppSelector } from "@/store/hooks";
import type { GB28181 } from "@/types";
import { DeviceListCard } from "./components/DeviceListCard";
import { Gb28181LivePlayer } from "./components/LivePlayer";
import { PtzPanel } from "./components/PtzPanel";
import { SessionDetailsCard } from "./components/SessionDetailsCard";
import { ptzCapabilityTag } from "./view";

const { Text, Title } = Typography;

export default function Gb28181Page() {
  const { message } = App.useApp();
  const canQuery = usePermission("iot:gb28181:query");
  const canControl = usePermission("iot:gb28181:control");
  const token = useAppSelector((state) => state.auth.token);

  const [keyword, setKeyword] = useState("");
  const [selectedDeviceId, setSelectedDeviceId] = useState<string>();
  const [selectedChannelId, setSelectedChannelId] = useState<string>();
  const [activeSession, setActiveSession] = useState<GB28181.PreviewStartResult | null>(null);
  const [ptzSpeed, setPtzSpeed] = useState(80);
  const activeSessionRef = useRef<GB28181.PreviewStartResult | null>(null);
  const tokenRef = useRef<string | null>(token);

  const healthQuery = useGb28181Health({ enabled: canQuery });
  const devicesQuery = useGb28181Devices({ enabled: canQuery });

  const catalogMutation = useGb28181CatalogQuery();
  const previewStartMutation = useGb28181PreviewStart();
  const previewStopMutation = useGb28181PreviewStop();
  const ptzMutation = useGb28181Ptz();

  const devices = devicesQuery.data?.items ?? [];

  useEffect(() => {
    tokenRef.current = token;
  }, [token]);

  const releaseActiveSession = useCallback(() => {
    const session = activeSessionRef.current;
    if (!session) return;
    activeSessionRef.current = null;
    stopPreviewKeepalive(session.session_id, tokenRef.current);
  }, []);

  useEffect(() => {
    activeSessionRef.current = activeSession;
  }, [activeSession]);

  useEffect(() => {
    const handlePageHide = () => releaseActiveSession();
    window.addEventListener("pagehide", handlePageHide);
    window.addEventListener("beforeunload", handlePageHide);
    return () => {
      window.removeEventListener("pagehide", handlePageHide);
      window.removeEventListener("beforeunload", handlePageHide);
      releaseActiveSession();
    };
  }, [releaseActiveSession]);

  const filteredDevices = useMemo(() => {
    const text = keyword.trim().toLowerCase();
    if (!text) return devices;
    return devices.filter((device) =>
      [
        device.id,
        device.name,
        device.manufacturer,
        device.remote_address,
        device.remote_ip,
        device.remote_port,
      ]
        .filter(Boolean)
        .some((item) => item.toLowerCase().includes(text))
    );
  }, [devices, keyword]);

  const selectedDevice = useMemo(
    () => devices.find((device) => device.id === selectedDeviceId) ?? devices[0],
    [devices, selectedDeviceId]
  );

  const channels = useMemo<GB28181.Channel[]>(() => {
    if (!selectedDevice) return [];
    if (selectedDevice.channels.length > 0) return selectedDevice.channels;
    return [
      {
        id: selectedDevice.id,
        name: selectedDevice.name || selectedDevice.id,
        manufacturer: selectedDevice.manufacturer,
        online: selectedDevice.online,
        ptz_type: -1,
        ptz_capable: false,
      },
    ];
  }, [selectedDevice]);

  const selectedChannel = useMemo(
    () => channels.find((channel) => channel.id === selectedChannelId) ?? channels[0],
    [channels, selectedChannelId]
  );

  const stats = useMemo(() => {
    const onlineDevices = devices.filter((device) => device.online).length;
    const channelCount = devices.reduce((sum, device) => sum + device.channels.length, 0);
    const onlineChannels = devices.reduce(
      (sum, device) => sum + device.channels.filter((channel) => channel.online).length,
      0
    );
    return { onlineDevices, channelCount, onlineChannels };
  }, [devices]);

  const selectedChannelSupportsPtz =
    selectedChannel?.ptz_type === undefined ||
    selectedChannel.ptz_type < 0 ||
    selectedChannel.ptz_capable;
  const ptzDisabled =
    !canControl || !selectedDevice || !selectedChannel || !selectedChannelSupportsPtz;

  const channelOptions = channels.map((channel) => ({
    label: `${channel.name || channel.id} (${channel.id})`,
    value: channel.id,
  }));

  const refreshAll = () => {
    healthQuery.refetch();
    devicesQuery.refetch();
  };

  const currentTarget = () => {
    if (!selectedDevice || !selectedChannel) {
      message.warning("请选择在线设备和通道");
      return null;
    }
    return { deviceId: selectedDevice.id, channelId: selectedChannel.id };
  };

  const handleQueryCatalog = () => {
    if (!selectedDevice) {
      message.warning("请选择设备");
      return;
    }
    catalogMutation.mutate(selectedDevice.id, {
      onSuccess: () => {
        window.setTimeout(() => devicesQuery.refetch(), 1200);
      },
    });
  };

  const handleStartPreview = () => {
    const target = currentTarget();
    if (!target) return;
    previewStartMutation.mutate(target, {
      onSuccess: (result) => {
        const previousSession = activeSessionRef.current;
        if (previousSession && previousSession.session_id !== result.session_id) {
          stopPreviewKeepalive(previousSession.session_id, tokenRef.current);
        }
        activeSessionRef.current = result;
        setActiveSession(result);
      },
    });
  };

  const handleStopSession = () => {
    if (!activeSession) {
      message.warning("当前没有活动会话");
      return;
    }
    previewStopMutation.mutate(
      { sessionId: activeSession.session_id },
      {
        onSuccess: () => {
          activeSessionRef.current = null;
          setActiveSession(null);
        },
      }
    );
  };

  const handlePtz = (action: GB28181.PtzAction) => {
    const target = currentTarget();
    if (!target) return;
    ptzMutation.mutate({ ...target, action, speed: ptzSpeed });
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有 GB28181 查询权限，请联系管理员。" />
      </PageContainer>
    );
  }

  const pageHeader = (
    <div className="flex flex-wrap items-center justify-between gap-3">
      <Space size={12} wrap>
        <Title level={4} className="!mb-0">
          GB28181
        </Title>
        {healthQuery.isError && <Tag color="error">模块异常</Tag>}
      </Space>
      <Space wrap>
        <Input.Search
          allowClear
          placeholder="搜索设备、编号、厂商、地址"
          value={keyword}
          onChange={(event) => setKeyword(event.target.value)}
          className="w-[280px]"
        />
        <Button icon={<ReloadOutlined />} loading={devicesQuery.isFetching} onClick={refreshAll}>
          刷新
        </Button>
      </Space>
    </div>
  );

  return (
    <PageContainer header={pageHeader}>
      <div className="grid grid-cols-1 2xl:grid-cols-[420px_minmax(0,1fr)] gap-4">
        <div className="space-y-4 min-w-0">
          <DeviceListCard
            devices={devices}
            filteredDevices={filteredDevices}
            selectedDevice={selectedDevice}
            stats={stats}
            loading={devicesQuery.isLoading}
            onSelect={(device) => {
              setSelectedDeviceId(device.id);
              setSelectedChannelId(undefined);
            }}
          />
        </div>

        <div className="space-y-4 min-w-0">
          <Card
            size="small"
            title={
              <Space wrap>
                <PlayCircleOutlined />
                实时预览
                {selectedDevice && <Tag>{selectedDevice.name || selectedDevice.id}</Tag>}
                {ptzCapabilityTag(selectedChannel)}
                <Select
                  size="small"
                  value={selectedChannel?.id}
                  placeholder="选择通道"
                  options={channelOptions}
                  disabled={!selectedDevice}
                  className="min-w-[260px]"
                  onChange={setSelectedChannelId}
                />
              </Space>
            }
            extra={
              <Space wrap>
                <Button
                  icon={<SendOutlined />}
                  disabled={!selectedDevice || !canControl}
                  loading={catalogMutation.isPending}
                  onClick={handleQueryCatalog}
                >
                  目录查询
                </Button>
                <Button
                  type="primary"
                  icon={<PlayCircleOutlined />}
                  disabled={!canControl}
                  loading={previewStartMutation.isPending}
                  onClick={handleStartPreview}
                >
                  开始预览
                </Button>
                <Button
                  danger
                  icon={<PauseCircleOutlined />}
                  disabled={!activeSession || !canControl}
                  loading={previewStopMutation.isPending}
                  onClick={handleStopSession}
                >
                  停止
                </Button>
              </Space>
            }
          >
            <div className="grid grid-cols-1 xl:grid-cols-[minmax(0,1fr)_280px] gap-4 items-start">
              <div className="relative aspect-video bg-black rounded overflow-hidden">
                {activeSession ? (
                  <Gb28181LivePlayer key={activeSession.session_id} session={activeSession} />
                ) : (
                  <div className="h-full flex flex-col items-center justify-center text-white/70">
                    <VideoCameraOutlined className="text-5xl mb-3" />
                    <Text className="!text-white/80">
                      {selectedDevice
                        ? `${selectedDevice.name || selectedDevice.id} / ${selectedChannel?.name || selectedChannel?.id || "未选择通道"}`
                        : "请选择设备和通道"}
                    </Text>
                  </div>
                )}
              </div>

              <PtzPanel
                speed={ptzSpeed}
                disabled={ptzDisabled}
                loading={ptzMutation.isPending}
                onSpeedChange={setPtzSpeed}
                onAction={handlePtz}
              />
            </div>
          </Card>

          <SessionDetailsCard
            selectedDevice={selectedDevice}
            selectedChannel={selectedChannel}
            activeSession={activeSession}
          />
        </div>
      </div>
    </PageContainer>
  );
}
