/**
 * 设备管理页面 - 以实时数据卡片为主，集成管理和历史数据功能
 */

import {
  ApartmentOutlined,
  DeleteOutlined,
  EditOutlined,
  HistoryOutlined,
  PictureOutlined,
  PlusOutlined,
  ReloadOutlined,
  SendOutlined,
} from "@ant-design/icons";
import type { MenuProps } from "antd";
import {
  App,
  Button,
  Card,
  Dropdown,
  Empty,
  Flex,
  Input,
  Popover,
  Result,
  Skeleton,
  Space,
  Tag,
  Tooltip,
} from "antd";
import {
  type CSSProperties,
  memo,
  type ReactNode,
  startTransition,
  useCallback,
  useMemo,
  useRef,
  useState,
} from "react";
import DeviceCard from "@/components/DeviceCard";
import ImagePreviewModal, { type ImagePreviewModalRef } from "@/components/ImagePreviewModal";
import { PageContainer } from "@/components/PageContainer";
import { useDebounceFn, usePermissions } from "@/hooks";
import { useWsStatus } from "@/providers";
import { useDeviceDelete, useDeviceList, useDeviceSave, useLinkOptions } from "@/services";
import type { Device } from "@/types";
import CommandPopover from "./CommandPopover";
import DeviceFormModal, { type DeviceFormValues } from "./DeviceFormModal";
import DeviceGroupPanel from "./DeviceGroupPanel";
import HistoryDataModal from "./HistoryDataModal";
import TopologyView from "./TopologyView";
import {
  calcWeightedLength,
  formatReportTime,
  isOnline,
  parseBitMapping,
  separatorClass,
} from "./utils";

const { Search } = Input;
const EMPTY_DEVICE_LIST: Device.RealTimeData[] = [];
const EMPTY_COMMAND_OPS: Device.CommandOperation[] = [];
const EMPTY_IMAGE_OPS: Device.ImageOperation[] = [];
const DEVICE_GRID_COLUMNS = 3;
const DEVICE_GRID_STYLE: CSSProperties = {
  gridTemplateColumns: `repeat(${DEVICE_GRID_COLUMNS}, 1fr)`,
};

interface DeviceProtocolStats {
  total: number;
  online: number;
  offline: number;
  enabled: number;
}

interface DeviceStats {
  total: number;
  online: number;
  offline: number;
  enabled: number;
  byProtocol: Record<string, DeviceProtocolStats>;
}

const createEmptyDeviceStats = (): DeviceStats => ({
  total: 0,
  online: 0,
  offline: 0,
  enabled: 0,
  byProtocol: {},
});

interface DeviceCardDisplayItem {
  key: number;
  label: string;
  children: ReactNode;
  span?: number;
}

const buildDeviceCardItems = (elements?: Device.Element[]): DeviceCardDisplayItem[] => {
  if (!elements) return [];

  return elements
    .map((el, idx) => {
      if (el.dictConfig && el.value !== null && el.value !== undefined && el.value !== "") {
        if (el.dictConfig.mapType === "VALUE") {
          const rawValue = String(el.value);
          const matchedItem = el.dictConfig.items.find(
            (item) => item && typeof item === "object" && item.key === rawValue
          );
          if (matchedItem) {
            return { key: idx, label: el.name, children: matchedItem.label };
          }
          return null;
        }

        if (el.dictConfig.mapType === "BIT") {
          const matchedLabels = parseBitMapping(el.value, el.dictConfig);
          if (matchedLabels.length === 0) return null;

          const totalTextLength = matchedLabels.reduce(
            (sum, label) => sum + calcWeightedLength(label),
            0
          );
          const labelLength = calcWeightedLength(el.name);
          const gapLength = matchedLabels.length > 1 ? 1.5 * (matchedLabels.length - 1) : 0;
          const totalLength = labelLength + totalTextLength + gapLength;
          const needFullRow = totalLength > 20;

          const children = (
            <Space key={idx} size={4}>
              {matchedLabels.map((label, i) => (
                <Tag key={i} color="blue">
                  {label}
                </Tag>
              ))}
            </Space>
          );

          return { key: idx, label: el.name, children, span: needFullRow ? 2 : undefined };
        }
      }

      const displayValue =
        el.value === null || el.value === undefined || el.value === "" ? "--" : el.value;
      const children =
        displayValue === "--" || !el.unit ? String(displayValue) : `${displayValue} ${el.unit}`;
      return { key: idx, label: el.name, children };
    })
    .filter((item): item is NonNullable<typeof item> => item !== null);
};

interface DeviceGridItemProps {
  device: Device.RealTimeData;
  canEdit: boolean;
  canDelete: boolean;
  isCommandPopoverOpen: boolean;
  activeCommandFunc: Device.CommandOperation | null;
  onImageClick: (imageOp: Device.ImageOperation) => void;
  onOpenCommandPopover: (device: Device.RealTimeData, op: Device.CommandOperation) => void;
  onCloseCommandPopover: () => void;
  onOpenHistoryModal: (device: Device.RealTimeData) => void;
  onOpenEditModal: (device: Device.RealTimeData) => void;
  onDeleteDevice: (device: Device.RealTimeData) => void;
}

const DeviceGridItem = memo(
  ({
    device,
    canEdit,
    canDelete,
    isCommandPopoverOpen,
    activeCommandFunc,
    onImageClick,
    onOpenCommandPopover,
    onCloseCommandPopover,
    onOpenHistoryModal,
    onOpenEditModal,
    onDeleteDevice,
  }: DeviceGridItemProps) => {
    const online = useMemo(
      () => isOnline(device.connected, device.reportTime, device.online_timeout),
      [device.connected, device.online_timeout, device.reportTime]
    );
    const items = useMemo(() => buildDeviceCardItems(device.elements), [device.elements]);
    const canRemoteControl = device.remote_control !== false;
    const commandOps = device.commandOperations ?? EMPTY_COMMAND_OPS;
    const imageOps = device.imageOperations ?? EMPTY_IMAGE_OPS;
    const hasImageData = useMemo(
      () => imageOps.some((op) => op.latestImage?.data),
      [imageOps]
    );
    const downMenuItems = useMemo<MenuProps["items"]>(
      () =>
        commandOps.map((op, idx) => ({
          key: String(idx),
          label: op.name,
        })),
      [commandOps]
    );
    const imageMenuItems = useMemo<MenuProps["items"]>(
      () =>
        imageOps.map((op, idx) => ({
          key: String(idx),
          label: op.name,
        })),
      [imageOps]
    );

    return (
      <div className="flex flex-col">
        <DeviceCard
          title={
            <Flex justify="space-between" className="w-full">
              <span>
                {device.name}
                {device.device_code ? `:${device.device_code}` : ""}
              </span>
              {online ? <Tag color="success">在线</Tag> : <Tag color="error">离线</Tag>}
            </Flex>
          }
          subtitle={
            <Flex justify="space-between" className="w-full">
              <span>
                <Tag color={device.link_id === 0 ? "orange" : "blue"} className="!mr-1">
                  {device.link_id === 0 ? "Agent" : device.link_name || "未绑定链路"}
                </Tag>
                <Tag color="purple">{device.protocol_name}</Tag>
              </span>
              <span className="text-gray-400 text-xs">
                上报：{formatReportTime(device.reportTime)}
              </span>
            </Flex>
          }
          items={items}
          column={2}
          length={20}
          extra={
            <Flex align="center" justify="space-around" className="w-full">
              {imageOps.length > 0 && (
                <>
                  <Dropdown
                    disabled={!hasImageData}
                    menu={{
                      items: imageMenuItems,
                      onClick: ({ key }) => {
                        const op = imageOps[Number(key)];
                        if (op) onImageClick(op);
                      },
                    }}
                  >
                    <Tooltip title={hasImageData ? "查看图片" : "暂无图片数据"}>
                      <Button
                        type="text"
                        size="small"
                        icon={<PictureOutlined />}
                        disabled={!hasImageData}
                      />
                    </Tooltip>
                  </Dropdown>

                  <span className={separatorClass} />
                </>
              )}

              <Popover
                open={isCommandPopoverOpen}
                trigger="click"
                placement="bottomRight"
                content={
                  isCommandPopoverOpen && activeCommandFunc ? (
                    <CommandPopover
                      device={device}
                      func={activeCommandFunc}
                      onClose={onCloseCommandPopover}
                    />
                  ) : null
                }
                onOpenChange={(open) => {
                  if (!open) onCloseCommandPopover();
                }}
              >
                <Dropdown
                  disabled={!commandOps.length || !canRemoteControl}
                  menu={{
                    items: downMenuItems,
                    onClick: ({ key }) => {
                      const op = commandOps[Number(key)];
                      if (op) onOpenCommandPopover(device, op);
                    },
                  }}
                >
                  <Tooltip
                    title={
                      !canRemoteControl
                        ? "该设备已禁止远控"
                        : !online
                          ? "设备离线（点击后将提示）"
                          : "下发指令"
                    }
                  >
                    <Button
                      type="text"
                      size="small"
                      icon={<SendOutlined />}
                      disabled={!commandOps.length || !canRemoteControl}
                    />
                  </Tooltip>
                </Dropdown>
              </Popover>

              <span className={separatorClass} />

              <Tooltip title="历史数据">
                <Button
                  type="text"
                  size="small"
                  icon={<HistoryOutlined />}
                  onClick={() => onOpenHistoryModal(device)}
                />
              </Tooltip>

              <span className={separatorClass} />

              {canEdit && (
                <Tooltip title="编辑设备">
                  <Button
                    type="text"
                    size="small"
                    icon={<EditOutlined />}
                    onClick={() => onOpenEditModal(device)}
                  />
                </Tooltip>
              )}

              {canDelete && (
                <>
                  <span className={separatorClass} />
                  <Tooltip title="删除设备">
                    <Button
                      type="text"
                      size="small"
                      danger
                      icon={<DeleteOutlined />}
                      onClick={() => onDeleteDevice(device)}
                    />
                  </Tooltip>
                </>
              )}
            </Flex>
          }
        />
      </div>
    );
  }
);

// ========== 主页面组件 ==========

const DevicePage = () => {
  const { modal, message } = App.useApp();
  const imageModalRef = useRef<ImagePreviewModalRef>(null);

  // 权限（单次调用，仅构建一次 Set）
  const { hasPermission } = usePermissions();
  const canQuery = hasPermission("iot:device:query");
  const canAdd = hasPermission("iot:device:add");
  const canEdit = hasPermission("iot:device:edit");
  const canDelete = hasPermission("iot:device:delete");
  const canManageGroup = hasPermission("iot:device-group:edit");

  // 搜索
  const [keyword, setKeyword] = useState("");

  // 分组筛选：null=全部, 0=未分组, >0=指定分组
  const [selectedGroupId, setSelectedGroupId] = useState<number | null>(null);

  // 拓扑视图
  const [topologyVisible, setTopologyVisible] = useState(false);

  // 设备表单弹窗
  const [formModalVisible, setFormModalVisible] = useState(false);
  const [editing, setEditing] = useState<Device.Item | null>(null);

  // 历史数据弹窗
  const [historyModalVisible, setHistoryModalVisible] = useState(false);
  const [historyDevice, setHistoryDevice] = useState<Device.RealTimeData | null>(null);

  // 指令下发
  const [commandPopoverOpen, setCommandPopoverOpen] = useState(false);
  const [commandDevice, setCommandDevice] = useState<Device.RealTimeData | null>(null);
  const [commandFunc, setCommandFunc] = useState<Device.CommandOperation | null>(null);

  // ========== Queries & Mutations ==========

  const { connected: wsConnected } = useWsStatus();
  const { data, isLoading, refetch } = useDeviceList({
    enabled: canQuery,
    pollingInterval: wsConnected ? false : 3000,
  });
  const { data: linkOptionsData } = useLinkOptions({ enabled: canQuery });
  const linkOptions = linkOptionsData?.list ?? [];

  const saveMutation = useDeviceSave();
  const deleteMutation = useDeviceDelete();

  const deviceList = data?.list ?? EMPTY_DEVICE_LIST;
  const { groupedDeviceList, stats, ungroupedCount } = useMemo(() => {
    const nextGroupedDeviceList: Device.RealTimeData[] = [];
    const nextStats = createEmptyDeviceStats();
    let nextUngroupedCount = 0;

    for (const device of deviceList) {
      if (!device.group_id) {
        nextUngroupedCount++;
      }

      const matchesGroup =
        selectedGroupId === null
          ? true
          : selectedGroupId === 0
            ? !device.group_id
            : device.group_id === selectedGroupId;

      if (!matchesGroup) {
        continue;
      }

      const isDeviceOnline = isOnline(
        device.connected,
        device.reportTime,
        device.online_timeout
      );
      if (isDeviceOnline) {
        nextStats.online++;
      } else {
        nextStats.offline++;
      }
      if (device.status === "enabled") {
        nextStats.enabled++;
      }

      nextStats.total++;
      const protocolName = device.protocol_type || device.protocol_name || "未知";
      if (!nextStats.byProtocol[protocolName]) {
        nextStats.byProtocol[protocolName] = { total: 0, online: 0, offline: 0, enabled: 0 };
      }
      nextStats.byProtocol[protocolName].total++;
      if (isDeviceOnline) {
        nextStats.byProtocol[protocolName].online++;
      } else {
        nextStats.byProtocol[protocolName].offline++;
      }
      if (device.status === "enabled") {
        nextStats.byProtocol[protocolName].enabled++;
      }

      nextGroupedDeviceList.push(device);
    }

    return {
      groupedDeviceList: nextGroupedDeviceList,
      stats: nextStats,
      ungroupedCount: nextUngroupedCount,
    };
  }, [deviceList, selectedGroupId]);

  const normalizedKeyword = keyword.trim().toLowerCase();
  const filteredDeviceList = useMemo(() => {
    if (!normalizedKeyword) {
      return groupedDeviceList;
    }

    return groupedDeviceList.filter((device) => {
      return (
        device.name?.toLowerCase().includes(normalizedKeyword) ||
        device.device_code?.toLowerCase().includes(normalizedKeyword) ||
        device.protocol_name?.toLowerCase().includes(normalizedKeyword)
      );
    });
  }, [groupedDeviceList, normalizedKeyword]);
  const protocolStatsEntries = useMemo(() => Object.entries(stats.byProtocol), [stats.byProtocol]);

  const columns = DEVICE_GRID_COLUMNS;

  // ========== 搜索 ==========

  const { run: debouncedSearch } = useDebounceFn((value: string) => {
    startTransition(() => setKeyword(value));
  }, 300);

  // ========== 设备表单 ==========

  const openCreateModal = () => {
    setEditing(null);
    setFormModalVisible(true);
  };

  const openEditModal = useCallback((device: Device.RealTimeData) => {
    setEditing({
      id: device.id,
      name: device.name,
      device_code: device.device_code,
      link_id: device.link_id,
      protocol_config_id: device.protocol_config_id,
      status: device.status,
      online_timeout: device.online_timeout,
      remote_control: device.remote_control,
      modbus_mode: device.modbus_mode,
      slave_id: device.slave_id,
      timezone: device.timezone,
      heartbeat: device.heartbeat,
      registration: device.registration,
      remark: device.remark,
      group_id: device.group_id,
      // Agent 模式字段
      agent_id: device.agent_id,
      agent_endpoint_id: device.agent_endpoint_id,
    });
    setFormModalVisible(true);
  }, []);

  const onDeleteDevice = useCallback(
    (device: Device.RealTimeData) => {
      modal.confirm({
        title: `确认删除设备「${device.name}」吗？`,
        content: "删除后设备将停止数据采集，历史数据仍会保留。此操作不可撤销。",
        okText: "确定删除",
        okButtonProps: { danger: true },
        onOk: () => deleteMutation.mutate(device.id),
      });
    },
    [deleteMutation, modal]
  );

  const onFormFinish = (values: DeviceFormValues) => {
    // 剔除前端辅助字段 connection_mode，构造后端 DTO
    const { connection_mode, ...rest } = values;
    saveMutation.mutate(rest as Device.CreateDto & { id?: number }, {
      onSuccess: () => {
        setFormModalVisible(false);
        setEditing(null);
        refetch();
      },
    });
  };

  // ========== 图片查看 ==========

  const handleImageClick = useCallback(
    (imageOp: Device.ImageOperation) => {
      if (imageOp.latestImage?.data) {
        imageModalRef.current?.open(imageOp.latestImage.data, imageOp.name);
      } else {
        message.info("暂无图片数据");
      }
    },
    [message]
  );

  // ========== 指令下发 ==========

  const openCommandPopover = useCallback((device: Device.RealTimeData, func: Device.CommandOperation) => {
    setCommandDevice(device);
    setCommandFunc(func);
    setCommandPopoverOpen(true);
  }, []);

  // ========== 历史数据 ==========

  const closeCommandPopover = useCallback(() => {
    setCommandPopoverOpen(false);
  }, []);

  const openHistoryModal = useCallback((device: Device.RealTimeData) => {
    setHistoryDevice(device);
    setHistoryModalVisible(true);
  }, []);

  const renderSkeletons = () => {
    return Array.from({ length: columns }).map((_, idx) => (
      <div key={idx} className="bg-white rounded-lg px-3.5 py-3">
        <Skeleton active title paragraph={{ rows: 4 }} />
      </div>
    ));
  };

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询设备列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  return (
    <PageContainer
      header={
        <div className="flex items-center justify-between flex-wrap gap-2">
          <h3 className="text-base font-medium m-0">设备管理</h3>
          <Space wrap>
            <DeviceGroupPanel
              selectedGroupId={selectedGroupId}
              onSelect={setSelectedGroupId}
              canManageGroup={canManageGroup}
              ungroupedCount={ungroupedCount}
            />
            <Search
              allowClear
              placeholder="设备名称 / 编码 / 类型"
              onChange={(e) => debouncedSearch(e.target.value)}
              className="w-60"
            />
            <Tooltip title="拓扑视图">
              <Button icon={<ApartmentOutlined />} onClick={() => setTopologyVisible(true)} />
            </Tooltip>
            <Tooltip title="刷新">
              <Button icon={<ReloadOutlined />} onClick={() => refetch()} loading={isLoading} />
            </Tooltip>
            {canAdd && (
              <Button type="primary" icon={<PlusOutlined />} onClick={openCreateModal}>
                新建设备
              </Button>
            )}
          </Space>
        </div>
      }
    >
      {/* 概况统计 */}
      <Flex gap={12} className="mb-3" wrap="wrap">
        <Card
          size="small"
          className="flex-1 min-w-[140px]"
          styles={{ body: { padding: "12px 16px" } }}
        >
          <Flex justify="space-between" align="center" className="mb-2.5">
            <span className="text-gray-500 text-[13px]">设备总数</span>
            <span className="text-lg font-semibold text-[#1677ff]">{stats.total}</span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {protocolStatsEntries.map(([protocol, data]) => (
              <Tag key={protocol} color="blue" className="!m-0 !px-3 !py-1 !text-sm !leading-5">
                {protocol}: {data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card
          size="small"
          className="flex-1 min-w-[140px]"
          styles={{ body: { padding: "12px 16px" } }}
        >
          <Flex justify="space-between" align="center" className="mb-2.5">
            <span className="text-gray-500 text-[13px]">在线设备</span>
            <span className="text-lg font-semibold text-[#52c41a]">
              {stats.online}
              <span className="text-[13px] text-gray-400 font-normal"> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {protocolStatsEntries.map(([protocol, data]) => (
              <Tag key={protocol} color="green" className="!m-0 !px-3 !py-1 !text-sm !leading-5">
                {protocol}: {data.online}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card
          size="small"
          className="flex-1 min-w-[140px]"
          styles={{ body: { padding: "12px 16px" } }}
        >
          <Flex justify="space-between" align="center" className="mb-2.5">
            <span className="text-gray-500 text-[13px]">离线设备</span>
            <span className="text-lg font-semibold text-[#ff4d4f]">
              {stats.offline}
              <span className="text-[13px] text-gray-400 font-normal"> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {protocolStatsEntries.map(([protocol, data]) => (
              <Tag key={protocol} color="red" className="!m-0 !px-3 !py-1 !text-sm !leading-5">
                {protocol}: {data.offline}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card
          size="small"
          className="flex-1 min-w-[140px]"
          styles={{ body: { padding: "12px 16px" } }}
        >
          <Flex justify="space-between" align="center" className="mb-2.5">
            <span className="text-gray-500 text-[13px]">已启用</span>
            <span className="text-lg font-semibold text-[#722ed1]">
              {stats.enabled}
              <span className="text-[13px] text-gray-400 font-normal"> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {protocolStatsEntries.map(([protocol, data]) => (
              <Tag key={protocol} color="purple" className="!m-0 !px-3 !py-1 !text-sm !leading-5">
                {protocol}: {data.enabled}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
      </Flex>

      {/* 设备卡片网格 */}
      <div className="grid gap-3" style={DEVICE_GRID_STYLE}>
        {isLoading && filteredDeviceList.length === 0 ? (
          renderSkeletons()
        ) : filteredDeviceList.length === 0 ? (
          <div className="col-span-full py-12">
            <Empty description={keyword ? "搜索无结果，请尝试调整关键词" : "暂无设备数据"} />
          </div>
        ) : (
          filteredDeviceList.map((device) => {
            return (
              <DeviceGridItem
                key={device.id}
                device={device}
                canEdit={canEdit}
                canDelete={canDelete}
                isCommandPopoverOpen={commandPopoverOpen && commandDevice?.id === device.id}
                activeCommandFunc={commandDevice?.id === device.id ? commandFunc : null}
                onImageClick={handleImageClick}
                onOpenCommandPopover={openCommandPopover}
                onCloseCommandPopover={closeCommandPopover}
                onOpenHistoryModal={openHistoryModal}
                onOpenEditModal={openEditModal}
                onDeleteDevice={onDeleteDevice}
              />
            );
          })
        )}
      </div>

      {/* 图片预览 */}
      <ImagePreviewModal ref={imageModalRef} />

      {/* 设备表单弹窗 */}
      <DeviceFormModal
        open={formModalVisible}
        editing={editing}
        loading={saveMutation.isPending}
        linkOptions={linkOptions}
        onCancel={() => {
          setFormModalVisible(false);
          setEditing(null);
        }}
        onFinish={onFormFinish}
      />

      {/* 历史数据弹窗 */}
      <HistoryDataModal
        open={historyModalVisible}
        device={historyDevice}
        onClose={() => {
          setHistoryModalVisible(false);
          setHistoryDevice(null);
        }}
      />

      {/* 拓扑视图 */}
      <TopologyView open={topologyVisible} onClose={() => setTopologyVisible(false)} />
    </PageContainer>
  );
};

export default DevicePage;
