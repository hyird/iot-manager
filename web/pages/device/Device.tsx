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
import {
  useDeviceDelete,
  useDeviceGroupTreeWithCount,
  useDeviceList,
  useDeviceSave,
  useLinkOptions,
} from "@/services";
import type { Device, DeviceGroup } from "@/types";
import CommandPopover from "./CommandPopover";
import DeviceFormModal, { type DeviceFormValues } from "./DeviceFormModal";
import DeviceGroupPanel from "./DeviceGroupPanel";
import HistoryDataModal from "./HistoryDataModal";
import TopologyView from "./TopologyView";
import {
  calcWeightedLength,
  formatElementValue,
  formatReportTime,
  getDeviceConnectionState,
  getDeviceStatusBadge,
  parseBitMapping,
  separatorClass,
} from "./utils";

const { Search } = Input;
const EMPTY_DEVICE_LIST: Device.RealTimeData[] = [];
const EMPTY_COMMAND_OPS: Device.CommandOperation[] = [];
const EMPTY_IMAGE_OPS: Device.ImageOperation[] = [];
const DEVICE_CARD_GRID_STYLE: CSSProperties = {
  gridTemplateColumns: "repeat(auto-fill, minmax(320px, 1fr))",
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

interface DeviceGroupStats {
  total: number;
  online: number;
  offline: number;
  enabled: number;
}

const createEmptyDeviceGroupStats = (): DeviceGroupStats => ({
  total: 0,
  online: 0,
  offline: 0,
  enabled: 0,
});

const accumulateDeviceStats = <
  T extends { total: number; online: number; offline: number; enabled: number },
>(
  stats: T,
  device: Device.RealTimeData
) => {
  const state = getDeviceConnectionState(
    device.connected,
    device.reportTime,
    device.online_timeout,
    device.connectionState
  );
  stats.total++;
  if (state === "online") {
    stats.online++;
  } else {
    stats.offline++;
  }
  if (device.status === "enabled") {
    stats.enabled++;
  }
};

const buildDeviceStats = (devices: Device.RealTimeData[]): DeviceStats => {
  const stats = createEmptyDeviceStats();

  for (const device of devices) {
    accumulateDeviceStats(stats, device);
    const protocolName = device.protocol_type || device.protocol_name || "未知";
    if (!stats.byProtocol[protocolName]) {
      stats.byProtocol[protocolName] = { total: 0, online: 0, offline: 0, enabled: 0 };
    }
    accumulateDeviceStats(stats.byProtocol[protocolName], device);
  }

  return stats;
};

const buildDeviceGroupIndex = (groups: DeviceGroup.TreeItem[]) => {
  const index = new Map<number, DeviceGroup.TreeItem>();

  const walk = (nodes: DeviceGroup.TreeItem[]) => {
    for (const node of nodes) {
      index.set(node.id, node);
      if (node.children?.length) {
        walk(node.children);
      }
    }
  };

  walk(groups);
  return index;
};

const buildGroupScopeIds = (group?: DeviceGroup.TreeItem) => {
  if (!group) return null;

  const scope = new Set<number>();
  const walk = (node: DeviceGroup.TreeItem) => {
    if (scope.has(node.id)) return;
    scope.add(node.id);
    node.children?.forEach(walk);
  };

  walk(group);
  return scope;
};

const buildDeviceGroupStatsMap = (
  groups: DeviceGroup.TreeItem[],
  visibleDeviceMap: Map<number, Device.RealTimeData[]>
) => {
  const statsMap = new Map<number, DeviceGroupStats>();

  const walk = (node: DeviceGroup.TreeItem): DeviceGroupStats => {
    const stats = createEmptyDeviceGroupStats();
    const directDevices = visibleDeviceMap.get(node.id) ?? [];

    for (const device of directDevices) {
      accumulateDeviceStats(stats, device);
    }

    for (const child of node.children ?? []) {
      const childStats = walk(child);
      stats.total += childStats.total;
      stats.online += childStats.online;
      stats.offline += childStats.offline;
      stats.enabled += childStats.enabled;
    }

    statsMap.set(node.id, stats);
    return stats;
  };

  for (const group of groups) {
    walk(group);
  }

  return statsMap;
};

interface DeviceCardDisplayItem {
  key: number;
  label: string;
  children: ReactNode;
  span?: number;
  group?: string;
}

const buildDeviceCardItems = (elements?: Device.Element[]): DeviceCardDisplayItem[] => {
  if (!elements) return [];

  return elements
    .map((el, idx) => {
      const group = el.group?.trim() || undefined;

      if (el.dictConfig && el.value !== null && el.value !== undefined && el.value !== "") {
        if (el.dictConfig.mapType === "VALUE") {
          const rawValue = String(el.value);
          const matchedItem = el.dictConfig.items.find(
            (item) => item && typeof item === "object" && item.key === rawValue
          );
          if (matchedItem) {
            return { key: idx, label: el.name, children: matchedItem.label, group };
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

          return { key: idx, label: el.name, children, span: needFullRow ? 2 : undefined, group };
        }
      }

      const displayValue = formatElementValue(el.value, el.decimals);
      const children =
        displayValue === "--" || !el.unit ? displayValue : `${displayValue} ${el.unit}`;
      return { key: idx, label: el.name, children, group };
    })
    .filter((item): item is NonNullable<typeof item> => item !== null);
};

interface DeviceGridItemProps {
  device: Device.RealTimeData;
  canEdit: boolean;
  canDelete: boolean;
  canCommand: boolean;
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
    canCommand,
    isCommandPopoverOpen,
    activeCommandFunc,
    onImageClick,
    onOpenCommandPopover,
    onCloseCommandPopover,
    onOpenHistoryModal,
    onOpenEditModal,
    onDeleteDevice,
  }: DeviceGridItemProps) => {
    const connectionState = useMemo(
      () =>
        getDeviceConnectionState(
          device.connected,
          device.reportTime,
          device.online_timeout,
          device.connectionState
        ),
      [device.connected, device.connectionState, device.online_timeout, device.reportTime]
    );
    const statusTag = getDeviceStatusBadge(connectionState);
    const items = useMemo(() => buildDeviceCardItems(device.elements), [device.elements]);
    const canRemoteControl = device.remote_control !== false;
    const commandOps = device.commandOperations ?? EMPTY_COMMAND_OPS;
    const imageOps = device.imageOperations ?? EMPTY_IMAGE_OPS;
    const hasImageData = useMemo(() => imageOps.some((op) => op.latestImage?.data), [imageOps]);
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
              <Tag color={statusTag.color}>{statusTag.label}</Tag>
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
                    <CommandPopover device={device} func={activeCommandFunc} onClose={onCloseCommandPopover} />
                  ) : null
                }
                onOpenChange={(open) => {
                  if (!open) onCloseCommandPopover();
                }}
              >
                <Dropdown
                  disabled={!canCommand || !commandOps.length || !canRemoteControl}
                  menu={{
                    items: downMenuItems,
                    onClick: ({ key }) => {
                      const op = commandOps[Number(key)];
                      if (op && canCommand) onOpenCommandPopover(device, op);
                    },
                  }}
                >
                  <Tooltip
                    title={
                      !canCommand
                        ? "当前账号没有设备下发权限"
                        : !canRemoteControl
                        ? "该设备已禁止远控"
                        : connectionState === "online"
                          ? "下发指令"
                          : "设备离线（点击后将提示）"
                    }
                  >
                    <Button
                      type="text"
                      size="small"
                      icon={<SendOutlined />}
                      disabled={!canCommand || !commandOps.length || !canRemoteControl}
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
  const canCommand = hasPermission("iot:device:command");

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
  const { data: deviceGroupTree = [] } = useDeviceGroupTreeWithCount({ enabled: canQuery });
  const groupIndex = useMemo(() => buildDeviceGroupIndex(deviceGroupTree), [deviceGroupTree]);
  const selectedGroupNode = useMemo(
    () => (selectedGroupId && selectedGroupId > 0 ? groupIndex.get(selectedGroupId) : undefined),
    [groupIndex, selectedGroupId]
  );
  const selectedGroupScopeIds = useMemo(() => {
    if (selectedGroupId === null) return null;
    if (selectedGroupId === 0) return new Set<number>();
    return buildGroupScopeIds(selectedGroupNode) ?? new Set<number>([selectedGroupId]);
  }, [selectedGroupId, selectedGroupNode]);
  const ungroupedCount = useMemo(
    () => deviceList.filter((device) => !device.group_id).length,
    [deviceList]
  );
  const scopedDeviceList = useMemo(() => {
    if (selectedGroupId === null) {
      return deviceList;
    }

    if (selectedGroupId === 0) {
      return deviceList.filter((device) => !device.group_id);
    }

    return deviceList.filter((device) => {
      if (!device.group_id) return false;
      return selectedGroupScopeIds?.has(device.group_id) ?? device.group_id === selectedGroupId;
    });
  }, [deviceList, selectedGroupId, selectedGroupScopeIds]);

  const normalizedKeyword = keyword.trim().toLowerCase();
  const filteredDeviceList = useMemo(() => {
    if (!normalizedKeyword) {
      return scopedDeviceList;
    }

    return scopedDeviceList.filter((device) => {
      return (
        device.name?.toLowerCase().includes(normalizedKeyword) ||
        device.device_code?.toLowerCase().includes(normalizedKeyword) ||
        device.protocol_name?.toLowerCase().includes(normalizedKeyword)
      );
    });
  }, [normalizedKeyword, scopedDeviceList]);
  const stats = useMemo(() => buildDeviceStats(scopedDeviceList), [scopedDeviceList]);
  const displayStats = useMemo(() => buildDeviceStats(filteredDeviceList), [filteredDeviceList]);
  const visibleUngroupedDevices = useMemo(
    () => filteredDeviceList.filter((device) => !device.group_id),
    [filteredDeviceList]
  );
  const visibleUngroupedStats = useMemo(
    () => buildDeviceStats(visibleUngroupedDevices),
    [visibleUngroupedDevices]
  );
  const protocolStatsEntries = useMemo(() => Object.entries(stats.byProtocol), [stats.byProtocol]);
  const visibleDeviceMap = useMemo(() => {
    const map = new Map<number, Device.RealTimeData[]>();

    for (const device of filteredDeviceList) {
      if (!device.group_id) continue;
      const list = map.get(device.group_id);
      if (list) {
        list.push(device);
      } else {
        map.set(device.group_id, [device]);
      }
    }

    return map;
  }, [filteredDeviceList]);
  const visibleGroupStatsMap = useMemo(
    () => buildDeviceGroupStatsMap(deviceGroupTree, visibleDeviceMap),
    [deviceGroupTree, visibleDeviceMap]
  );
  const groupRoots = useMemo(() => {
    if (selectedGroupId === 0) return [];
    if (selectedGroupId !== null) {
      return selectedGroupNode ? [selectedGroupNode] : [];
    }
    return deviceGroupTree;
  }, [deviceGroupTree, selectedGroupId, selectedGroupNode]);

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

  const openCommandPopover = useCallback(
    (device: Device.RealTimeData, func: Device.CommandOperation) => {
      if (!canCommand) return;
      setCommandDevice(device);
      setCommandFunc(func);
      setCommandPopoverOpen(true);
    },
    [canCommand]
  );

  // ========== 历史数据 ==========

  const closeCommandPopover = useCallback(() => {
    setCommandPopoverOpen(false);
  }, []);

  const openHistoryModal = useCallback((device: Device.RealTimeData) => {
    setHistoryDevice(device);
    setHistoryModalVisible(true);
  }, []);

  const renderDeviceCards = (devices: Device.RealTimeData[]) => (
    <div className="mt-4 grid gap-3" style={DEVICE_CARD_GRID_STYLE}>
      {devices.map((device) => (
        <DeviceGridItem
          key={device.id}
          device={device}
          canEdit={canEdit}
          canDelete={canDelete}
          canCommand={canCommand}
          isCommandPopoverOpen={commandPopoverOpen && commandDevice?.id === device.id}
          activeCommandFunc={commandDevice?.id === device.id ? commandFunc : null}
          onImageClick={handleImageClick}
          onOpenCommandPopover={openCommandPopover}
          onCloseCommandPopover={closeCommandPopover}
          onOpenHistoryModal={openHistoryModal}
          onOpenEditModal={openEditModal}
          onDeleteDevice={onDeleteDevice}
        />
      ))}
    </div>
  );

  const renderSectionStats = (sectionStats: DeviceGroupStats) => (
    <Space size={6} wrap>
      <Tag color="blue">{sectionStats.total} 个</Tag>
      {sectionStats.online > 0 && <Tag color="green">{sectionStats.online} 在线</Tag>}
      {sectionStats.offline > 0 && <Tag color="red">{sectionStats.offline} 离线</Tag>}
      {sectionStats.enabled > 0 && <Tag color="purple">{sectionStats.enabled} 已启用</Tag>}
    </Space>
  );

  const renderGroupSection = (group: DeviceGroup.TreeItem, depth = 0): ReactNode => {
    const sectionStats = visibleGroupStatsMap.get(group.id);
    if (!sectionStats || sectionStats.total === 0) {
      return null;
    }

    const directDevices = visibleDeviceMap.get(group.id) ?? [];
    const childSections = (group.children ?? [])
      .map((child) => renderGroupSection(child, depth + 1))
      .filter((item): item is ReactNode => item !== null && item !== undefined && item !== false);

    return (
      <section
        key={group.id}
        className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4"
        style={depth > 0 ? { marginLeft: depth * 16 } : undefined}
      >
        <Flex justify="space-between" align="center" gap={12} wrap>
          <div className="min-w-0">
            <div className="text-sm font-semibold text-slate-800">{group.name}</div>
            <div className="mt-1 text-xs text-slate-500">
              同一分组内的设备会集中展示，便于批量维护
            </div>
          </div>
          {renderSectionStats(sectionStats)}
        </Flex>

        {directDevices.length > 0 ? renderDeviceCards(directDevices) : null}

        {childSections.length > 0 ? (
          <Space direction="vertical" className="mt-4 w-full" size="middle">
            {childSections}
          </Space>
        ) : null}
      </section>
    );
  };

  const renderUngroupedSection = (devices: Device.RealTimeData[]) => {
    if (devices.length === 0) return null;

    return (
      <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
        <Flex justify="space-between" align="center" gap={12} wrap>
          <div className="min-w-0">
            <div className="text-sm font-semibold text-slate-800">未分组</div>
            <div className="mt-1 text-xs text-slate-500">
              没有绑定设备分组的卡片会统一在这里展示
            </div>
          </div>
          {renderSectionStats(visibleUngroupedStats)}
        </Flex>

        {renderDeviceCards(devices)}
      </section>
    );
  };

  const renderFallbackSection = (devices: Device.RealTimeData[]) => {
    if (devices.length === 0) return null;

    const fallbackTitle =
      selectedGroupId === null
        ? "全部设备"
        : selectedGroupId === 0
          ? "未分组"
          : (groupIndex.get(selectedGroupId)?.name ?? `分组 #${selectedGroupId}`);

    const fallbackDescription =
      selectedGroupId === null
        ? "设备分组树尚未加载完成，先按当前条件展示全部设备"
        : "当前分组暂未展开，先展示匹配到的设备卡片";

    return (
      <section className="rounded-2xl border border-slate-200 bg-slate-50/70 p-4">
        <Flex justify="space-between" align="center" gap={12} wrap>
          <div className="min-w-0">
            <div className="text-sm font-semibold text-slate-800">{fallbackTitle}</div>
            <div className="mt-1 text-xs text-slate-500">{fallbackDescription}</div>
          </div>
          {renderSectionStats(displayStats)}
        </Flex>

        {renderDeviceCards(devices)}
      </section>
    );
  };

  const renderSkeletons = (count = 4) =>
    Array.from({ length: count }).map((_, idx) => (
      <div key={idx} className="rounded-lg bg-white px-3.5 py-3">
        <Skeleton active title paragraph={{ rows: 4 }} />
      </div>
    ));

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

      {/* 设备卡片分组展示 */}
      {isLoading && filteredDeviceList.length === 0 ? (
        <div className="grid gap-3" style={DEVICE_CARD_GRID_STYLE}>
          {renderSkeletons()}
        </div>
      ) : filteredDeviceList.length === 0 ? (
        <div className="py-12">
          <Empty description={keyword ? "搜索无结果，请尝试调整关键词" : "暂无设备数据"} />
        </div>
      ) : (
        <Space direction="vertical" className="w-full" size="large">
          {groupRoots.length > 0 ? (
            <>
              {groupRoots.map((group) => renderGroupSection(group))}
              {selectedGroupId === null && visibleUngroupedDevices.length > 0
                ? renderUngroupedSection(visibleUngroupedDevices)
                : null}
            </>
          ) : selectedGroupId === 0 ? (
            (renderUngroupedSection(visibleUngroupedDevices) ?? (
              <div className="py-12">
                <Empty description="暂无未分组设备" />
              </div>
            ))
          ) : (
            renderFallbackSection(filteredDeviceList)
          )}
        </Space>
      )}

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
