import {
  ArrowDownOutlined,
  ArrowLeftOutlined,
  ArrowRightOutlined,
  ArrowUpOutlined,
  CameraOutlined,
  DatabaseOutlined,
  MinusOutlined,
  PauseCircleOutlined,
  PlayCircleOutlined,
  PlusOutlined,
  ReloadOutlined,
  SendOutlined,
  StopOutlined,
  VideoCameraOutlined,
} from "@ant-design/icons";
import {
  App,
  Badge,
  Button,
  Card,
  DatePicker,
  Descriptions,
  Empty,
  Input,
  Result,
  Segmented,
  Slider,
  Space,
  Statistic,
  Table,
  Tabs,
  Tag,
  Tooltip,
  Typography,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import dayjs, { type Dayjs } from "dayjs";
import { type ReactNode, useEffect, useMemo, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission } from "@/hooks";
import {
  useGb28181CatalogQuery,
  useGb28181Devices,
  useGb28181Health,
  useGb28181MockRegister,
  useGb28181PlaybackStart,
  useGb28181PreviewStart,
  useGb28181PreviewStop,
  useGb28181Ptz,
  useGb28181RecordQuery,
  useGb28181SipConfig,
  useGb28181Streams,
} from "@/services";
import type { GB28181 } from "@/types";

const { RangePicker } = DatePicker;
const { Text, Title } = Typography;

type WorkMode = "preview" | "playback";

const DEFAULT_MOCK_DEVICE_ID = "34020000001320000001";

const PTZ_ACTIONS: Array<{
  action: GB28181.PtzAction;
  title: string;
  icon: ReactNode;
  className?: string;
}> = [
  { action: "up", title: "上", icon: <ArrowUpOutlined />, className: "col-start-2" },
  { action: "left", title: "左", icon: <ArrowLeftOutlined />, className: "col-start-1" },
  { action: "stop", title: "停止", icon: <StopOutlined />, className: "col-start-2" },
  { action: "right", title: "右", icon: <ArrowRightOutlined />, className: "col-start-3" },
  { action: "down", title: "下", icon: <ArrowDownOutlined />, className: "col-start-2" },
];

const formatGbTime = (value: Dayjs) => value.format("YYYY-MM-DDTHH:mm:ss");

const onlineTag = (online: boolean) => (
  <Badge status={online ? "processing" : "default"} text={online ? "在线" : "离线"} />
);

const displayText = (value?: string | number | null) => {
  if (value === undefined || value === null || value === "") return "--";
  return value;
};

function openExternal(url: string) {
  window.open(url, "_blank", "noopener,noreferrer");
}

export default function Gb28181Page() {
  const { message } = App.useApp();
  const canQuery = usePermission("iot:gb28181:query");
  const canControl = usePermission("iot:gb28181:control");
  const canRecord = usePermission("iot:gb28181:record");

  const [keyword, setKeyword] = useState("");
  const [selectedDeviceId, setSelectedDeviceId] = useState<string>();
  const [selectedChannelId, setSelectedChannelId] = useState<string>();
  const [activeSession, setActiveSession] = useState<GB28181.PreviewStartResult | null>(null);
  const [activeMode, setActiveMode] = useState<WorkMode>("preview");
  const [ptzSpeed, setPtzSpeed] = useState(80);
  const [mockDeviceId, setMockDeviceId] = useState(DEFAULT_MOCK_DEVICE_ID);
  const [recordRange, setRecordRange] = useState<[Dayjs, Dayjs]>([
    dayjs().subtract(1, "day"),
    dayjs(),
  ]);

  const healthQuery = useGb28181Health({ enabled: canQuery });
  const sipQuery = useGb28181SipConfig({ enabled: canQuery });
  const devicesQuery = useGb28181Devices({ enabled: canQuery });
  const streamsQuery = useGb28181Streams({ enabled: canQuery });

  const mockRegisterMutation = useGb28181MockRegister();
  const catalogMutation = useGb28181CatalogQuery();
  const previewStartMutation = useGb28181PreviewStart();
  const previewStopMutation = useGb28181PreviewStop();
  const ptzMutation = useGb28181Ptz();
  const recordQueryMutation = useGb28181RecordQuery();
  const playbackStartMutation = useGb28181PlaybackStart();

  const devices = devicesQuery.data?.items ?? [];
  const streams = streamsQuery.data?.items ?? [];

  const filteredDevices = useMemo(() => {
    const text = keyword.trim().toLowerCase();
    if (!text) return devices;
    return devices.filter((device) =>
      [device.id, device.name, device.manufacturer, device.remote_address]
        .filter(Boolean)
        .some((item) => item.toLowerCase().includes(text))
    );
  }, [devices, keyword]);

  const selectedDevice = useMemo(
    () => devices.find((device) => device.id === selectedDeviceId),
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
      },
    ];
  }, [selectedDevice]);

  const selectedChannel = useMemo(
    () => channels.find((channel) => channel.id === selectedChannelId),
    [channels, selectedChannelId]
  );

  const records = useMemo(() => {
    if (!selectedDevice) return [];
    if (!selectedChannelId) return selectedDevice.records;
    const filtered = selectedDevice.records.filter(
      (record) => record.device_id === selectedChannelId || record.device_id === selectedDevice.id
    );
    return filtered.length > 0 ? filtered : selectedDevice.records;
  }, [selectedDevice, selectedChannelId]);

  const stats = useMemo(() => {
    const onlineDevices = devices.filter((device) => device.online).length;
    const channelCount = devices.reduce((sum, device) => sum + device.channels.length, 0);
    const onlineChannels = devices.reduce(
      (sum, device) => sum + device.channels.filter((channel) => channel.online).length,
      0
    );
    const onlineStreams = streams.filter((stream) => stream.online).length;
    return { onlineDevices, channelCount, onlineChannels, onlineStreams };
  }, [devices, streams]);

  useEffect(() => {
    if (devices.length === 0) {
      setSelectedDeviceId(undefined);
      return;
    }
    if (!selectedDeviceId || !devices.some((device) => device.id === selectedDeviceId)) {
      setSelectedDeviceId(devices[0].id);
    }
  }, [devices, selectedDeviceId]);

  useEffect(() => {
    if (!selectedDevice) {
      setSelectedChannelId(undefined);
      return;
    }
    const nextChannelId = channels[0]?.id ?? selectedDevice.id;
    if (!selectedChannelId || !channels.some((channel) => channel.id === selectedChannelId)) {
      setSelectedChannelId(nextChannelId);
    }
  }, [channels, selectedChannelId, selectedDevice]);

  const refreshAll = () => {
    healthQuery.refetch();
    sipQuery.refetch();
    devicesQuery.refetch();
    streamsQuery.refetch();
  };

  const currentTarget = () => {
    if (!selectedDevice || !selectedChannelId) {
      message.warning("请选择在线设备和通道");
      return null;
    }
    return { deviceId: selectedDevice.id, channelId: selectedChannelId };
  };

  const handleMockRegister = () => {
    const value = mockDeviceId.trim();
    if (!value) {
      message.warning("请输入设备国标编号");
      return;
    }
    mockRegisterMutation.mutate(value, {
      onSuccess: () => {
        window.setTimeout(() => devicesQuery.refetch(), 300);
      },
    });
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
    previewStartMutation.mutate(
      {
        ...target,
        previousSessionId: activeMode === "preview" ? activeSession?.session_id : undefined,
      },
      {
        onSuccess: (result) => {
          setActiveMode("preview");
          setActiveSession(result);
          window.setTimeout(() => streamsQuery.refetch(), 600);
        },
      }
    );
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
          setActiveSession(null);
          window.setTimeout(() => streamsQuery.refetch(), 600);
        },
      }
    );
  };

  const handlePtz = (action: GB28181.PtzAction) => {
    const target = currentTarget();
    if (!target) return;
    ptzMutation.mutate({ ...target, action, speed: ptzSpeed });
  };

  const handleQueryRecords = () => {
    const target = currentTarget();
    if (!target) return;
    recordQueryMutation.mutate(
      {
        ...target,
        startTime: formatGbTime(recordRange[0]),
        endTime: formatGbTime(recordRange[1]),
      },
      {
        onSuccess: () => {
          window.setTimeout(() => devicesQuery.refetch(), 1500);
        },
      }
    );
  };

  const handleStartPlayback = (record?: GB28181.RecordItem) => {
    const target = currentTarget();
    if (!target) return;
    playbackStartMutation.mutate(
      {
        deviceId: target.deviceId,
        channelId: record?.device_id || target.channelId,
        startTime: record?.start_time || formatGbTime(recordRange[0]),
        endTime: record?.end_time || formatGbTime(recordRange[1]),
      },
      {
        onSuccess: (result) => {
          setActiveMode("playback");
          setActiveSession(result);
          window.setTimeout(() => streamsQuery.refetch(), 600);
        },
      }
    );
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有 GB28181 查询权限，请联系管理员。" />
      </PageContainer>
    );
  }

  const deviceColumns: ColumnsType<GB28181.Device> = [
    {
      title: "状态",
      dataIndex: "online",
      width: 78,
      render: (online: boolean) => onlineTag(online),
    },
    {
      title: "设备",
      dataIndex: "name",
      ellipsis: true,
      render: (name: string, record) => (
        <Space direction="vertical" size={0}>
          <Text strong>{displayText(name)}</Text>
          <Text type="secondary" className="text-xs">
            {record.id}
          </Text>
        </Space>
      ),
    },
    {
      title: "通道",
      width: 64,
      align: "right",
      render: (_, record) => record.channels.length,
    },
  ];

  const channelColumns: ColumnsType<GB28181.Channel> = [
    {
      title: "状态",
      dataIndex: "online",
      width: 78,
      render: (online: boolean) => onlineTag(online),
    },
    {
      title: "通道",
      dataIndex: "name",
      ellipsis: true,
      render: (name: string, record) => (
        <Space direction="vertical" size={0}>
          <Text strong>{displayText(name)}</Text>
          <Text type="secondary" className="text-xs">
            {record.id}
          </Text>
        </Space>
      ),
    },
    {
      title: "操作",
      width: 80,
      render: (_, record) => (
        <Button
          size="small"
          type={record.id === selectedChannelId ? "primary" : "default"}
          onClick={() => setSelectedChannelId(record.id)}
        >
          选择
        </Button>
      ),
    },
  ];

  const recordColumns: ColumnsType<GB28181.RecordItem> = [
    {
      title: "录像",
      dataIndex: "name",
      ellipsis: true,
      render: (name: string, record) => (
        <Space direction="vertical" size={0}>
          <Text>{displayText(name)}</Text>
          <Text type="secondary" className="text-xs">
            {displayText(record.device_id)}
          </Text>
        </Space>
      ),
    },
    { title: "开始", dataIndex: "start_time", width: 170 },
    { title: "结束", dataIndex: "end_time", width: 170 },
    { title: "类型", dataIndex: "type", width: 90, render: (value: string) => displayText(value) },
    {
      title: "操作",
      width: 90,
      fixed: "right",
      render: (_, record) => (
        <Button
          size="small"
          icon={<PlayCircleOutlined />}
          disabled={!canRecord}
          loading={playbackStartMutation.isPending}
          onClick={() => handleStartPlayback(record)}
        >
          回放
        </Button>
      ),
    },
  ];

  const streamColumns: ColumnsType<GB28181.StreamStatus> = [
    {
      title: "状态",
      dataIndex: "online",
      width: 78,
      render: (online: boolean) => onlineTag(online),
    },
    { title: "流 ID", dataIndex: "stream", ellipsis: true },
    { title: "应用", dataIndex: "app", width: 90 },
    { title: "协议", dataIndex: "schema", width: 90 },
    { title: "读者", dataIndex: "reader_count", width: 80, align: "right" },
  ];

  const playUrlEntries = activeSession
    ? [
        ["HLS", activeSession.play_urls.hls],
        ["HTTP-TS", activeSession.play_urls.http_ts],
        ["HTTP-FLV", activeSession.play_urls.http_flv],
        ["WebRTC", activeSession.play_urls.webrtc],
        ["RTSP", activeSession.play_urls.rtsp],
        ["WS-FLV", activeSession.play_urls.ws_flv],
      ].filter(([, url]) => Boolean(url))
    : [];
  const videoSource =
    activeSession?.play_urls.hls ||
    activeSession?.play_urls.http_ts ||
    activeSession?.play_urls.http_flv ||
    "";

  const pageHeader = (
    <div className="flex flex-wrap items-center justify-between gap-3">
      <Space size={12} wrap>
        <Title level={4} className="!mb-0">
          GB28181
        </Title>
        <Tag color={healthQuery.isError ? "error" : "success"}>
          {healthQuery.isError ? "模块异常" : healthQuery.data?.status || "运行中"}
        </Tag>
        <Text type="secondary">
          SIP {displayText(sipQuery.data?.id)} @ {displayText(sipQuery.data?.domain)}
        </Text>
        <Text type="secondary">
          {displayText(sipQuery.data?.host)}:{displayText(sipQuery.data?.port)}
        </Text>
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
          <Card
            size="small"
            title={
              <Space>
                <VideoCameraOutlined />
                设备列表
              </Space>
            }
            extra={
              <Tag color="blue">
                {stats.onlineDevices}/{devices.length}
              </Tag>
            }
          >
            <div className="grid grid-cols-2 gap-3 mb-3">
              <Statistic
                title="通道在线"
                value={stats.onlineChannels}
                suffix={`/ ${stats.channelCount}`}
                valueStyle={{ fontSize: 20 }}
              />
              <Statistic
                title="活动流"
                value={stats.onlineStreams}
                suffix={`/ ${streams.length}`}
                valueStyle={{ fontSize: 20 }}
              />
            </div>
            <Table
              rowKey="id"
              size="small"
              columns={deviceColumns}
              dataSource={filteredDevices}
              loading={devicesQuery.isLoading}
              pagination={{ pageSize: 10, size: "small" }}
              onRow={(record) => ({
                onClick: () => setSelectedDeviceId(record.id),
                className:
                  record.id === selectedDeviceId ? "cursor-pointer bg-blue-50" : "cursor-pointer",
              })}
            />

            <div className="mt-3">
              <Text type="secondary" className="text-xs">
                模拟设备
              </Text>
              <Space.Compact block className="mt-1">
                <Input
                  size="small"
                  value={mockDeviceId}
                  disabled={!canControl}
                  onChange={(event) => setMockDeviceId(event.target.value)}
                />
                <Button
                  size="small"
                  icon={<PlusOutlined />}
                  disabled={!canControl}
                  loading={mockRegisterMutation.isPending}
                  onClick={handleMockRegister}
                >
                  写入
                </Button>
              </Space.Compact>
            </div>
          </Card>

          <Card
            size="small"
            title={
              <Space>
                <CameraOutlined />
                通道
              </Space>
            }
            extra={
              <Button
                size="small"
                icon={<SendOutlined />}
                disabled={!selectedDevice || !canControl}
                loading={catalogMutation.isPending}
                onClick={handleQueryCatalog}
              >
                目录查询
              </Button>
            }
          >
            {selectedDevice ? (
              <Table
                rowKey="id"
                size="small"
                columns={channelColumns}
                dataSource={channels}
                pagination={{ pageSize: 5, size: "small" }}
                onRow={(record) => ({
                  onClick: () => setSelectedChannelId(record.id),
                  className:
                    record.id === selectedChannelId ? "cursor-pointer bg-blue-50" : "cursor-pointer",
                })}
              />
            ) : (
              <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} />
            )}
          </Card>
        </div>

        <div className="space-y-4 min-w-0">
          <Card
            size="small"
            title={
              <Space>
                <PlayCircleOutlined />
                {activeMode === "playback" ? "录像回放" : "实时预览"}
                {selectedDevice && <Tag>{selectedDevice.name || selectedDevice.id}</Tag>}
                {selectedChannel && <Tag color="blue">{selectedChannel.name || selectedChannel.id}</Tag>}
              </Space>
            }
            extra={
              <Space wrap>
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
            <div className="aspect-video bg-black rounded overflow-hidden">
              {activeSession ? (
                <video
                  key={videoSource}
                  src={videoSource}
                  controls
                  autoPlay
                  muted
                  className="w-full h-full"
                />
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
          </Card>

          <div className="grid grid-cols-1 xl:grid-cols-[320px_minmax(0,1fr)] gap-4">
            <Card size="small" title="云台">
              <div className="space-y-4">
                <div>
                  <Text type="secondary">速度</Text>
                  <Slider
                    min={1}
                    max={255}
                    value={ptzSpeed}
                    onChange={(value) => setPtzSpeed(Number(value))}
                  />
                </div>
                <div className="grid grid-cols-3 gap-2 w-[210px] mx-auto">
                  {PTZ_ACTIONS.map((item) => (
                    <Tooltip title={item.title} key={item.action}>
                      <Button
                        className={item.className}
                        icon={item.icon}
                        disabled={!canControl}
                        loading={ptzMutation.isPending}
                        onClick={() => handlePtz(item.action)}
                      />
                    </Tooltip>
                  ))}
                </div>
                <Space.Compact block>
                  <Button
                    icon={<PlusOutlined />}
                    disabled={!canControl}
                    loading={ptzMutation.isPending}
                    onClick={() => handlePtz("zoomin")}
                  >
                    变倍+
                  </Button>
                  <Button
                    icon={<MinusOutlined />}
                    disabled={!canControl}
                    loading={ptzMutation.isPending}
                    onClick={() => handlePtz("zoomout")}
                  >
                    变倍-
                  </Button>
                </Space.Compact>
              </div>
            </Card>

            <Card size="small" title="当前会话">
              <Descriptions size="small" column={{ xs: 1, lg: 2 }}>
                <Descriptions.Item label="设备">{displayText(selectedDevice?.id)}</Descriptions.Item>
                <Descriptions.Item label="通道">{displayText(selectedChannel?.id)}</Descriptions.Item>
                <Descriptions.Item label="远端">{displayText(selectedDevice?.remote_address)}</Descriptions.Item>
                <Descriptions.Item label="状态">
                  {selectedDevice ? onlineTag(selectedDevice.online) : "--"}
                </Descriptions.Item>
                <Descriptions.Item label="会话">
                  {displayText(activeSession?.session_id)}
                </Descriptions.Item>
                <Descriptions.Item label="RTP">
                  {displayText(activeSession?.rtp_port)}
                </Descriptions.Item>
              </Descriptions>
              <div className="mt-3 space-y-2">
                {playUrlEntries.length > 0 ? (
                  playUrlEntries.map(([label, url]) => (
                    <div key={label} className="flex items-center gap-2 min-w-0">
                      <Tag className="w-[72px] text-center">{label}</Tag>
                      <Text copyable={{ text: url }} ellipsis className="flex-1">
                        {url}
                      </Text>
                      <Button size="small" onClick={() => openExternal(url)}>
                        打开
                      </Button>
                    </div>
                  ))
                ) : (
                  <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="暂无播放地址" />
                )}
              </div>
            </Card>
          </div>

          <Tabs
            size="small"
            items={[
              {
                key: "records",
                label: "录像",
                children: (
                  <Card
                    size="small"
                    title={
                      <Space>
                        <DatabaseOutlined />
                        录像查询
                      </Space>
                    }
                    extra={
                      <Space wrap>
                        <RangePicker
                          showTime
                          allowClear={false}
                          value={recordRange}
                          onChange={(values) => {
                            if (values?.[0] && values[1]) {
                              setRecordRange([values[0], values[1]]);
                            }
                          }}
                        />
                        <Button
                          icon={<SendOutlined />}
                          disabled={!canRecord}
                          loading={recordQueryMutation.isPending}
                          onClick={handleQueryRecords}
                        >
                          查询
                        </Button>
                        <Button
                          type="primary"
                          icon={<PlayCircleOutlined />}
                          disabled={!canRecord}
                          loading={playbackStartMutation.isPending}
                          onClick={() => handleStartPlayback()}
                        >
                          按时间回放
                        </Button>
                      </Space>
                    }
                  >
                    <Table
                      rowKey={(record, index) =>
                        `${record.device_id}-${record.start_time}-${record.end_time}-${index}`
                      }
                      size="small"
                      columns={recordColumns}
                      dataSource={records}
                      pagination={{ pageSize: 8, size: "small" }}
                      scroll={{ x: 760 }}
                    />
                  </Card>
                ),
              },
              {
                key: "streams",
                label: "流状态",
                children: (
                  <Card size="small" title="ZLM 流状态">
                    <Table
                      rowKey={(record) => `${record.app}-${record.stream}-${record.schema}`}
                      size="small"
                      columns={streamColumns}
                      dataSource={streams}
                      loading={streamsQuery.isLoading}
                      pagination={{ pageSize: 10, size: "small" }}
                    />
                  </Card>
                ),
              },
              {
                key: "config",
                label: "配置",
                children: (
                  <Card size="small" title="SIP 配置">
                    <Descriptions size="small" bordered column={{ xs: 1, md: 2 }}>
                      <Descriptions.Item label="服务">
                        {displayText(healthQuery.data?.service)}
                      </Descriptions.Item>
                      <Descriptions.Item label="状态">
                        {healthQuery.isError ? "异常" : displayText(healthQuery.data?.status)}
                      </Descriptions.Item>
                      <Descriptions.Item label="域">
                        {displayText(sipQuery.data?.domain)}
                      </Descriptions.Item>
                      <Descriptions.Item label="国标 ID">
                        {displayText(sipQuery.data?.id)}
                      </Descriptions.Item>
                      <Descriptions.Item label="监听地址">
                        {displayText(sipQuery.data?.host)}
                      </Descriptions.Item>
                      <Descriptions.Item label="端口">
                        {displayText(sipQuery.data?.port)}
                      </Descriptions.Item>
                      <Descriptions.Item label="公网 IP">
                        {displayText(sipQuery.data?.public_ip)}
                      </Descriptions.Item>
                      <Descriptions.Item label="传输">
                        <Segmented
                          size="small"
                          value={String(displayText(sipQuery.data?.transport)).toUpperCase()}
                          options={[String(displayText(sipQuery.data?.transport)).toUpperCase()]}
                        />
                      </Descriptions.Item>
                    </Descriptions>
                  </Card>
                ),
              },
            ]}
          />
        </div>
      </div>
    </PageContainer>
  );
}
