/**
 * 设备管理页面 - 以实时数据卡片为主，集成管理和历史数据功能
 */

import { useState, useCallback, useRef, useMemo, useEffect } from "react";
import {
  Button,
  Form,
  Input,
  InputNumber,
  Modal,
  Select,
  Space,
  Switch,
  Table,
  Tag,
  App,
  Result,
  Skeleton,
  Dropdown,
  Popover,
  Checkbox,
  Flex,
  DatePicker,
  Image,
  Tooltip,
  Card,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import type { MenuProps } from "antd";
import {
  EditOutlined,
  DeleteOutlined,
  HistoryOutlined,
  PictureOutlined,
  SendOutlined,
  PlusOutlined,
  ReloadOutlined,
} from "@ant-design/icons";
import { useDebounceFn } from "ahooks";
import dayjs from "dayjs";
import type { Device, HistoryDataType } from "@/types";
import { usePermission } from "@/hooks";
import { PageContainer } from "@/components/PageContainer";
import DeviceCard from "@/components/DeviceCard";
import ImagePreviewModal, { type ImagePreviewModalRef } from "@/components/ImagePreviewModal";
import { useDeviceList, useDeviceSave, useDeviceDelete, useDeviceCommand } from "@/services";
import { useLinkOptions, useProtocolConfigOptions, deviceApi } from "@/services";

const { Search } = Input;
const { RangePicker } = DatePicker;

// ========== 类型定义 ==========

interface DeviceFormValues {
  id?: number;
  name: string;
  device_code: string;
  link_id: number;
  protocol_config_id: number;
  status: Device.Status;
  /** 在线超时时间（秒） */
  online_timeout?: number;
  /** 是否允许远控 */
  remote_control?: boolean;
  remark?: string;
}

interface HistoryFuncRow extends Device.HistoryFunc {
  key: string;
  deviceCode: string;
}

interface FuncState {
  loading: boolean;
  list: Device.HistoryFunc[];
  page: number;
  pageSize: number;
  total: number;
}

interface RecordState {
  loading: boolean;
  dataType: HistoryDataType;
  list: Device.HistoryElement[] | Device.HistoryImage[];
  page: number;
  pageSize: number;
  total: number;
}

// ========== 工具函数 ==========

/** 获取默认时间范围（最近一周） */
const getDefaultTimeRange = () => [dayjs().subtract(7, "day").startOf("day"), dayjs().endOf("day")];

/** 默认在线超时时间（秒） */
const DEFAULT_ONLINE_TIMEOUT = 300;

/** 综合判断设备在线状态（取心跳时间和上报时间中较新的） */
const isOnline = (lastHeartbeatTime?: string, reportTime?: string, onlineTimeout?: number) => {
  const now = Date.now();
  const threshold = (onlineTimeout || DEFAULT_ONLINE_TIMEOUT) * 1000; // 转为毫秒

  const heartbeatTs = lastHeartbeatTime ? new Date(lastHeartbeatTime).getTime() : 0;
  const reportTs = reportTime ? new Date(reportTime).getTime() : 0;

  // 取较新的时间
  const latestTs = Math.max(heartbeatTs, reportTs);
  if (latestTs === 0) return false;

  return now - latestTs < threshold;
};

const formatReportTime = (reportTime?: string) => {
  if (!reportTime) return "--";
  // 后端返回的时间是设备本地时间（UTC+8），格式可能是 "2025-01-29 10:22:15" 或 ISO 格式
  // 使用 dayjs 解析，如果是 ISO 格式带 Z 后缀，需要转换为本地时间显示
  const t = dayjs(reportTime);
  if (!t.isValid()) return "--";
  return t.format("YYYY-MM-DD HH:mm:ss");
};

const makeRecordKey = (code: string, funcCode: string, dataType: HistoryDataType) =>
  `${code}|${funcCode}|${dataType}`;

/** 解析位映射字典配置 */
const parseBitMapping = (
  value: string | number,
  dictConfig: NonNullable<Device.Element["dictConfig"]>
) => {
  // 将值转换为数字（处理十六进制字符串）
  let numValue: number;
  if (typeof value === "string") {
    numValue =
      value.startsWith("0x") || /^[0-9A-Fa-f]+$/.test(value)
        ? parseInt(value, 16)
        : parseInt(value, 10);
  } else {
    numValue = value;
  }

  if (isNaN(numValue)) return [];

  const matchedLabels: string[] = [];
  for (const item of dictConfig.items) {
    // 过滤掉 null 或无效的映射项
    if (!item || typeof item !== "object") continue;

    const bitIndex = parseInt(item.key, 10);
    if (isNaN(bitIndex) || bitIndex < 0 || bitIndex > 31) continue;

    // 检查依赖条件（如果有）
    if (item.dependsOn && item.dependsOn.conditions && item.dependsOn.conditions.length > 0) {
      const { operator, conditions } = item.dependsOn;
      let anyMet = false;
      let allMet = true;

      for (const dep of conditions) {
        // 过滤掉 null 或无效的依赖条件
        if (!dep || typeof dep !== "object" || dep.bitIndex === undefined) {
          allMet = false;
          continue;
        }

        const depBitIndex = parseInt(dep.bitIndex, 10);
        if (isNaN(depBitIndex) || depBitIndex < 0 || depBitIndex > 31) {
          allMet = false;
          continue;
        }

        const depBitValue = (numValue >> depBitIndex) & 1;
        const depExpectedValue = dep.bitValue === "1" ? 1 : 0;
        const conditionMet = depBitValue === depExpectedValue;

        if (conditionMet) anyMet = true;
        if (!conditionMet) allMet = false;
      }

      // 根据运算符判断依赖条件是否满足
      const dependencyMet = operator === "OR" ? anyMet : allMet;
      if (!dependencyMet) {
        continue; // 依赖条件不满足，跳过该映射项
      }
    }

    // 获取该位的值
    const bitValue = (numValue >> bitIndex) & 1;
    // 触发值，默认为 "1"
    const triggerValue = item.value || "1";
    // 检查是否匹配
    const shouldTrigger =
      (triggerValue === "1" && bitValue === 1) || (triggerValue === "0" && bitValue === 0);

    if (shouldTrigger) {
      matchedLabels.push(item.label);
    }
  }

  return matchedLabels;
};

// ========== 主页面组件 ==========

const DevicePage = () => {
  const { modal, message } = App.useApp();
  const imageModalRef = useRef<ImagePreviewModalRef>(null);

  // 权限
  const canQuery = usePermission("iot:device:query");
  const canAdd = usePermission("iot:device:add");
  const canEdit = usePermission("iot:device:edit");
  const canDelete = usePermission("iot:device:delete");

  // 搜索
  const [keyword, setKeyword] = useState("");

  // 列数
  const [columns, setColumns] = useState(3);

  // 设备表单弹窗
  const [formModalVisible, setFormModalVisible] = useState(false);
  const [editing, setEditing] = useState<Device.Item | null>(null);
  const [form] = Form.useForm<DeviceFormValues>();

  // 历史数据弹窗
  const [historyModalVisible, setHistoryModalVisible] = useState(false);
  const [historyDevice, setHistoryDevice] = useState<Device.RealTimeData | null>(null);

  // 指令下发
  const [commandPopoverOpen, setCommandPopoverOpen] = useState(false);
  const [commandDevice, setCommandDevice] = useState<Device.RealTimeData | null>(null);
  const [commandFunc, setCommandFunc] = useState<Device.DownFunc | null>(null);
  const [commandElements, setCommandElements] = useState<
    Array<{
      _key: string;
      elementId: number;
      name: string;
      value: string;
      unit?: string;
      options?: Device.DownFuncElement["options"];
    }>
  >([]);
  const [commandSelectedKeys, setCommandSelectedKeys] = useState<string[]>([]);

  // 历史数据状态
  const [historyForm] = Form.useForm();
  const [funcMap, setFuncMap] = useState<Record<string, FuncState>>({});
  const [expandedFuncKeys, setExpandedFuncKeys] = useState<React.Key[]>([]);
  const [recordMap, setRecordMap] = useState<Record<string, RecordState>>({});

  // ========== Queries & Mutations ==========

  const { data, isLoading, refetch } = useDeviceList({ enabled: canQuery });
  const { data: linkOptions = [] } = useLinkOptions({ enabled: canQuery });
  const { data: protocolOptions } = useProtocolConfigOptions("SL651", { enabled: canQuery });

  const saveMutation = useDeviceSave();
  const deleteMutation = useDeviceDelete();
  const commandMutation = useDeviceCommand();

  const deviceList = useMemo(() => data?.list || [], [data?.list]);

  // 过滤设备列表
  const filteredDeviceList = useMemo(() => {
    if (!keyword) return deviceList;
    const lowerKeyword = keyword.toLowerCase();
    return deviceList.filter(
      (d) =>
        d.deviceName?.toLowerCase().includes(lowerKeyword) ||
        d.code?.toLowerCase().includes(lowerKeyword) ||
        d.typeName?.toLowerCase().includes(lowerKeyword)
    );
  }, [deviceList, keyword]);

  // 统计数据
  const stats = useMemo(() => {
    const total = deviceList.length;
    let online = 0;
    let offline = 0;
    let enabled = 0;
    const byProtocol: Record<
      string,
      { total: number; online: number; offline: number; enabled: number }
    > = {};

    deviceList.forEach((d) => {
      const isDeviceOnline = isOnline(d.lastHeartbeatTime, d.reportTime, d.online_timeout);
      if (isDeviceOnline) {
        online++;
      } else {
        offline++;
      }
      if (d.status === "enabled") {
        enabled++;
      }

      // 按协议类型统计
      const protocolName = d.protocol_type || d.typeName || "未知";
      if (!byProtocol[protocolName]) {
        byProtocol[protocolName] = { total: 0, online: 0, offline: 0, enabled: 0 };
      }
      byProtocol[protocolName].total++;
      if (isDeviceOnline) {
        byProtocol[protocolName].online++;
      } else {
        byProtocol[protocolName].offline++;
      }
      if (d.status === "enabled") {
        byProtocol[protocolName].enabled++;
      }
    });

    return { total, online, offline, enabled, byProtocol };
  }, [deviceList]);

  // 响应式列数
  useEffect(() => {
    const calcColumns = () => {
      const width = window.innerWidth;
      if (width <= 768) setColumns(1);
      else if (width <= 1199) setColumns(2);
      else setColumns(3);
    };
    calcColumns();
    window.addEventListener("resize", calcColumns);
    return () => window.removeEventListener("resize", calcColumns);
  }, []);

  // ========== 搜索 ==========

  const { run: debouncedSearch } = useDebounceFn((value: string) => setKeyword(value), {
    wait: 300,
  });

  // ========== 设备表单 ==========

  const openCreateModal = () => {
    setEditing(null);
    form.resetFields();
    form.setFieldsValue({ status: "enabled", remote_control: true });
    setFormModalVisible(true);
  };

  const openEditModal = (device: Device.RealTimeData) => {
    setEditing({
      id: device.id,
      name: device.name,
      device_code: device.device_code,
      link_id: device.link_id,
      protocol_config_id: device.protocol_config_id,
      status: device.status,
      online_timeout: device.online_timeout,
      remote_control: device.remote_control,
      remark: device.remark,
    });
    form.setFieldsValue({
      id: device.id,
      name: device.name,
      device_code: device.device_code,
      link_id: device.link_id,
      protocol_config_id: device.protocol_config_id,
      status: device.status,
      online_timeout: device.online_timeout,
      remote_control: device.remote_control ?? true,
      remark: device.remark,
    });
    setFormModalVisible(true);
  };

  const onDeleteDevice = (device: Device.RealTimeData) => {
    modal.confirm({
      title: `确认删除设备「${device.deviceName}」吗？`,
      onOk: () => deleteMutation.mutate(device.id),
    });
  };

  const onFormFinish = (values: DeviceFormValues) => {
    saveMutation.mutate(values as Device.CreateDto & { id?: number }, {
      onSuccess: () => {
        setFormModalVisible(false);
        setEditing(null);
        form.resetFields();
        refetch();
      },
    });
  };

  // ========== 图片查看 ==========

  const handleImageClick = useCallback(
    (imageFunc: Device.ImageFunc) => {
      if (imageFunc.latestImage?.data) {
        imageModalRef.current?.open(imageFunc.latestImage.data, imageFunc.name);
      } else {
        message.info("暂无图片数据");
      }
    },
    [message]
  );

  // ========== 指令下发 ==========

  const openCommandPopover = useCallback((device: Device.RealTimeData, func: Device.DownFunc) => {
    setCommandDevice(device);
    setCommandFunc(func);
    const list = (func.elements || []).map((el) => ({
      ...el,
      _key: String(el.elementId ?? el.name),
      value: el.value ?? "",
    }));
    setCommandElements(list);
    setCommandSelectedKeys(list.map((el) => el._key));
    setCommandPopoverOpen(true);
  }, []);

  const handleSendCommand = useCallback(() => {
    if (!commandDevice || !commandFunc) return;
    const elementsToSend = commandElements.filter((el) => commandSelectedKeys.includes(el._key));
    if (!elementsToSend.length) {
      message.warning("请至少选择一个要素");
      return;
    }
    const linkId = commandDevice.linkId;
    if (!linkId) {
      message.error("缺少链路ID");
      return;
    }

    // 检查设备是否在线
    const online = isOnline(
      commandDevice.lastHeartbeatTime,
      commandDevice.reportTime,
      commandDevice.online_timeout
    );
    if (!online) {
      message.error("设备离线，无法下发指令");
      return;
    }

    commandMutation.mutate(
      {
        linkId,
        payload: {
          deviceCode: commandDevice.code,
          funcCode: commandFunc.funcCode,
          elements: elementsToSend.map((el) => ({ elementId: el.elementId, value: el.value })),
        },
      },
      { onSuccess: () => setCommandPopoverOpen(false) }
    );
  }, [commandDevice, commandFunc, commandElements, commandSelectedKeys, commandMutation, message]);

  // ========== 历史数据 ==========

  const openHistoryModal = (device: Device.RealTimeData) => {
    setHistoryDevice(device);
    setFuncMap({});
    setRecordMap({});
    setExpandedFuncKeys([]);
    historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
    setHistoryModalVisible(true);
    // 自动展开第一个功能码并加载数据
    fetchFuncList(device.code, undefined, undefined, true);
  };

  const getTimeRangeParams = () => {
    const values = historyForm.getFieldsValue();
    let startTime: Date;
    let endTime: Date;
    if (values.timeRange?.length === 2) {
      startTime = new Date(values.timeRange[0].toISOString());
      endTime = new Date(values.timeRange[1].toISOString());
    } else {
      // 如果没有时间范围，使用默认值（最近一周）
      const defaultRange = getDefaultTimeRange();
      startTime = new Date(defaultRange[0].toISOString());
      endTime = new Date(defaultRange[1].toISOString());
    }
    return { startTime, endTime };
  };

  const fetchFuncList = async (code: string, pageArg?: number, pageSizeArg?: number, autoExpand = false) => {
    const prev = funcMap[code];
    const current = pageArg ?? prev?.page ?? 1;
    const size = pageSizeArg ?? prev?.pageSize ?? 10;

    setFuncMap((prevState) => ({
      ...prevState,
      [code]: {
        ...(prevState[code] ?? { list: [], page: current, pageSize: size, total: 0 }),
        loading: true,
      },
    }));

    try {
      const { startTime, endTime } = getTimeRangeParams();
      const res = await deviceApi.getHistoryData({
        code,
        page: current,
        pageSize: size,
        startTime,
        endTime,
      });
      const list = (res.list as Device.HistoryFunc[]) || [];
      setFuncMap((prevState) => ({
        ...prevState,
        [code]: {
          loading: false,
          list,
          page: current,
          pageSize: size,
          total: res.total || list.length || 0,
        },
      }));

      // 自动展开并加载第一个功能码的数据
      if (autoExpand && list.length > 0) {
        const firstFunc = list[0];
        const key = `${firstFunc.funcCode}-${firstFunc.dataType}`;
        setExpandedFuncKeys([key]);
        fetchFuncRecords({
          code,
          funcCode: firstFunc.funcCode,
          dataType: firstFunc.dataType,
        });
      }
    } catch {
      setFuncMap((prevState) => ({
        ...prevState,
        [code]: {
          ...(prevState[code] ?? { list: [], page: current, pageSize: size, total: 0 }),
          loading: false,
        },
      }));
    }
  };

  const fetchFuncRecords = async (opts: {
    code: string;
    funcCode: string;
    dataType: HistoryDataType;
    pageArg?: number;
    pageSizeArg?: number;
  }) => {
    const { code, funcCode, dataType } = opts;
    const key = makeRecordKey(code, funcCode, dataType);
    const prev = recordMap[key];
    const current = opts.pageArg ?? prev?.page ?? 1;
    const size = opts.pageSizeArg ?? prev?.pageSize ?? 10;

    setRecordMap((prevState) => ({
      ...prevState,
      [key]: prevState[key]
        ? { ...prevState[key], loading: true }
        : { loading: true, dataType, list: [], page: current, pageSize: size, total: 0 },
    }));

    try {
      const { startTime, endTime } = getTimeRangeParams();
      const res = await deviceApi.getHistoryData({
        code,
        funcCode,
        dataType,
        page: current,
        pageSize: size,
        startTime,
        endTime,
      });
      setRecordMap((prevState) => ({
        ...prevState,
        [key]: {
          loading: false,
          dataType,
          list:
            dataType === "ELEMENT"
              ? (res.list as Device.HistoryElement[])
              : (res.list as Device.HistoryImage[]),
          page: current,
          pageSize: size,
          total: res.total || res.list?.length || 0,
        },
      }));
    } catch {
      setRecordMap((prevState) => ({
        ...prevState,
        [key]: prevState[key]
          ? { ...prevState[key], loading: false }
          : { loading: false, dataType, list: [], page: current, pageSize: size, total: 0 },
      }));
    }
  };

  // ========== 渲染 ==========

  if (!canQuery) {
    return (
      <PageContainer>
        <Result status="403" title="无权限" subTitle="您没有查询设备列表的权限，请联系管理员" />
      </PageContainer>
    );
  }

  const convertElements = (elements?: Device.Element[]) => {
    if (!elements) return [];

    // 辅助函数：计算加权长度（中文字符1.5，其他字符1）
    const calcWeightedLength = (str: string) => {
      let len = 0;
      for (const ch of str) {
        if (/[\u4e00-\u9fa5]/.test(ch)) len += 1.5;
        else len += 1;
      }
      return len;
    };

    // 处理每个要素
    const converted = elements
      .map((el, idx) => {
        // 如果有字典配置，进行映射
        if (el.dictConfig && el.value !== null && el.value !== undefined && el.value !== "") {
          if (el.dictConfig.mapType === "VALUE") {
            // 值映射：匹配原始值
            const rawValue = String(el.value);
            // ✅ 过滤掉 null 或无效的映射项
            const matchedItem = el.dictConfig.items.find(
              (item) => item && typeof item === "object" && item.key === rawValue
            );
            if (matchedItem) {
              return { key: idx, label: el.name, children: matchedItem.label };
            }
            // 没有匹配到，不显示
            return null;
          } else if (el.dictConfig.mapType === "BIT") {
            // 位映射：解析二进制位
            const matchedLabels = parseBitMapping(el.value, el.dictConfig);
            if (matchedLabels.length === 0) {
              // 没有匹配的位，不显示
              return null;
            }

            // 计算所有 Tag 文本的总长度（包括标签名）
            const totalTextLength = matchedLabels.reduce(
              (sum, label) => sum + calcWeightedLength(label),
              0
            );
            const labelLength = calcWeightedLength(el.name);
            // Tag 之间的间隙：1.5 * (tag数量 - 1)
            const gapLength = matchedLabels.length > 1 ? 1.5 * (matchedLabels.length - 1) : 0;
            const totalLength = labelLength + totalTextLength + gapLength;

            // 根据总长度决定是否独占一行（length=20 是调用 DeviceCard 时传入的值）
            const needFullRow = totalLength > 20;

            // 多个值用 Tag 显示，横向排列
            const children = (
              <Space size={4}>
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

        // 没有字典配置，显示原始值
        const displayValue =
          el.value === null || el.value === undefined || el.value === "" ? "--" : el.value;
        const children =
          displayValue === "--" || !el.unit ? String(displayValue) : `${displayValue} ${el.unit}`;
        return { key: idx, label: el.name, children };
      })
      .filter((item): item is NonNullable<typeof item> => item !== null);

    return converted;
  };

  const renderCommandPopoverContent = () => {
    if (!commandDevice || !commandFunc) return null;
    if (!commandElements.length) return <div style={{ padding: 12 }}>暂无可下发要素</div>;

    return (
      <div style={{ maxWidth: 360 }}>
        <div style={{ marginBottom: 8 }}>
          <div>
            设备：{commandDevice.deviceName}（{commandDevice.code}）
          </div>
          <div style={{ fontSize: 12, color: "#999" }}>
            指令：{commandFunc.name}（{commandFunc.funcCode}）
          </div>
        </div>
        <div style={{ maxHeight: 260, overflowY: "auto", paddingRight: 4, marginBottom: 8 }}>
          {commandElements.map((el) => {
            const key = el._key;
            const checked = commandSelectedKeys.includes(key);
            const hasOptions = el.options && el.options.length > 0;
            return (
              <div
                key={key}
                style={{
                  marginBottom: 8,
                  paddingBottom: 8,
                  borderBottom: hasOptions ? "1px solid #f0f0f0" : "none",
                }}
              >
                <Flex align="center" style={{ marginBottom: hasOptions ? 6 : 0 }}>
                  <Checkbox
                    checked={checked}
                    onChange={(e) =>
                      setCommandSelectedKeys((prev) =>
                        e.target.checked ? [...prev, key] : prev.filter((k) => k !== key)
                      )
                    }
                  />
                  <span style={{ flex: 1, marginLeft: 6, marginRight: 6 }}>
                    {el.name}
                    {el.unit ? `（${el.unit}）` : ""}
                  </span>
                  <Input
                    size="small"
                    style={{ width: 120 }}
                    value={el.value}
                    placeholder={hasOptions ? "或手动输入" : ""}
                    onChange={(e) =>
                      setCommandElements((prev) =>
                        prev.map((item) =>
                          item._key === key ? { ...item, value: e.target.value } : item
                        )
                      )
                    }
                  />
                </Flex>
                {hasOptions && (
                  <Flex wrap gap={6} style={{ marginLeft: 26 }}>
                    <span style={{ fontSize: 12, color: "#999", marginRight: 4 }}>预设值：</span>
                    {el.options!.map((opt) => (
                      <Button
                        key={opt.value}
                        size="small"
                        type="primary"
                        ghost
                        loading={commandMutation.isPending}
                        onClick={() => {
                          // 检查设备是否在线
                          const online = isOnline(
                            commandDevice.lastHeartbeatTime,
                            commandDevice.reportTime,
                            commandDevice.online_timeout
                          );
                          if (!online) {
                            message.error("设备离线，无法下发指令");
                            return;
                          }
                          // 直接下发该预设值
                          const linkId = commandDevice.linkId;
                          if (!linkId) {
                            message.error("缺少链路ID");
                            return;
                          }
                          commandMutation.mutate({
                            linkId,
                            payload: {
                              deviceCode: commandDevice.code,
                              funcCode: commandFunc.funcCode,
                              elements: [{ elementId: el.elementId, value: opt.value }],
                            },
                          });
                        }}
                      >
                        {opt.label}
                      </Button>
                    ))}
                  </Flex>
                )}
              </div>
            );
          })}
        </div>
        <Flex justify="flex-end" gap={8}>
          <Button size="small" onClick={() => setCommandPopoverOpen(false)}>
            取消
          </Button>
          <Button
            size="small"
            type="primary"
            loading={commandMutation.isPending}
            disabled={!commandSelectedKeys.length}
            onClick={handleSendCommand}
          >
            下发
          </Button>
        </Flex>
      </div>
    );
  };

  const renderSkeletons = () => {
    return Array.from({ length: columns }).map((_, idx) => (
      <div
        key={idx}
        style={{
          flex: `0 0 calc(${100 / columns}% - 12px)`,
          background: "#fff",
          borderRadius: 8,
          padding: "12px 14px",
        }}
      >
        <Skeleton active title paragraph={{ rows: 4 }} />
      </div>
    ));
  };

  // 渲染单个要素值（支持字典映射）
  const renderElementValue = (el: Device.Element | undefined) => {
    if (!el) return "---";

    // 如果有字典配置，应用映射
    if (el.dictConfig && el.value !== null && el.value !== undefined && el.value !== "") {
      if (el.dictConfig.mapType === "VALUE") {
        // 值映射
        const rawValue = String(el.value);
        const matchedItem = el.dictConfig.items.find(
          (item) => item && typeof item === "object" && item.key === rawValue
        );
        if (matchedItem) {
          return matchedItem.label;
        }
        // 没有匹配到，显示原始值
        return el.unit ? `${el.value} ${el.unit}` : String(el.value);
      } else if (el.dictConfig.mapType === "BIT") {
        // 位映射
        const matchedLabels = parseBitMapping(el.value, el.dictConfig);
        if (matchedLabels.length > 0) {
          return (
            <Space size={4} wrap>
              {matchedLabels.map((label, i) => (
                <Tag key={i} color="blue">
                  {label}
                </Tag>
              ))}
            </Space>
          );
        }
        // 没有匹配的位，显示原始值
        return el.unit ? `${el.value} ${el.unit}` : String(el.value);
      }
    }

    // 没有字典配置，显示原始值
    return el.unit ? `${el.value} ${el.unit}` : (el.value ?? "---");
  };

  // 历史数据表格渲染
  const renderElementTable = (func: HistoryFuncRow) => {
    const key = makeRecordKey(func.deviceCode, func.funcCode, func.dataType);
    const rs = recordMap[key];
    const records = (rs?.list as Device.HistoryElement[]) || [];
    const elementNameSet = new Set<string>();
    records.forEach((r) => r.elements.forEach((e) => elementNameSet.add(e.name)));
    const elementNames = Array.from(elementNameSet);

    // 检查是否有下行数据（用于决定是否显示发送人和应答列）
    const hasDownData = records.some((r) => r.direction === "DOWN");

    // 收集应答要素名称（用于显示应答列）
    const responseElementNames = new Set<string>();
    records.forEach((r) => {
      if (r.responseElements) {
        r.responseElements.forEach((e) => responseElementNames.add(e.name));
      }
    });

    type RowType = {
      key: string;
      reportTime: string;
      elements: Device.Element[];
      direction?: "UP" | "DOWN";
      responseId?: number;
      responseElements?: Device.Element[];
      userId?: number;
      userName?: string;
    };
    const dataSource: RowType[] = records.map((r, idx) => ({
      key: `${idx}_${new Date(r.reportTime).getTime()}`,
      reportTime: new Date(r.reportTime).toLocaleString(),
      elements: r.elements,
      direction: r.direction,
      responseId: r.responseId,
      responseElements: r.responseElements,
      userId: r.userId,
      userName: r.userName,
    }));

    const cols: ColumnsType<RowType> = [
      { title: "时间", dataIndex: "reportTime", key: "reportTime", fixed: "left", width: 180 },
      {
        title: "方向",
        dataIndex: "direction",
        key: "direction",
        width: 80,
        render: (dir?: "UP" | "DOWN") =>
          dir === "DOWN" ? (
            <Tag color="orange">下行</Tag>
          ) : (
            <Tag color="green">上行</Tag>
          ),
      },
      // 只有存在下行数据时才显示发送人列
      ...(hasDownData
        ? [
            {
              title: "发送人",
              dataIndex: "userName",
              key: "userName",
              width: 100,
              render: (_: unknown, record: RowType) =>
                record.direction === "DOWN" ? record.userName || "-" : "-",
            },
          ]
        : []),
      ...elementNames.map((name) => ({
        title: name,
        dataIndex: "elements",
        key: name,
        width: 150,
        ellipsis: true,
        render: (els: RowType["elements"]) => {
          const el = els.find((e) => e.name === name);
          return renderElementValue(el);
        },
      })),
      // 显示应答列（仅下行数据有应答）
      ...(hasDownData
        ? [
            {
              title: "应答状态",
              key: "responseStatus",
              width: 100,
              render: (_: unknown, record: RowType) => {
                if (record.direction !== "DOWN") return "-";
                return record.responseId ? (
                  <Tag color="success">已应答</Tag>
                ) : (
                  <Tag color="warning">待应答</Tag>
                );
              },
            },
            // 应答要素列（动态生成）
            ...Array.from(responseElementNames).map((name) => ({
              title: `应答-${name}`,
              key: `response_${name}`,
              width: 150,
              ellipsis: true,
              render: (_: unknown, record: RowType) => {
                if (record.direction !== "DOWN" || !record.responseElements) return "-";
                const el = record.responseElements.find((e) => e.name === name);
                return renderElementValue(el);
              },
            })),
          ]
        : []),
    ];

    // 计算表格宽度
    const baseWidth = 180 + 80; // 时间 + 方向
    const elementsWidth = elementNames.length * 150;
    const downWidth = hasDownData ? 100 + 100 + Array.from(responseElementNames).length * 150 : 0; // 发送人 + 应答状态 + 应答要素
    const tableWidth = Math.max(baseWidth + elementsWidth + downWidth, 800);

    return (
      <Table<RowType>
        size="small"
        bordered
        loading={rs?.loading}
        columns={cols}
        dataSource={dataSource}
        scroll={{ x: tableWidth }}
        pagination={{
          current: rs?.page ?? 1,
          pageSize: rs?.pageSize ?? 10,
          total: rs?.total ?? 0,
          showSizeChanger: true,
          showTotal: (t) => `共 ${t} 条`,
          onChange: (p, ps) =>
            fetchFuncRecords({
              code: func.deviceCode,
              funcCode: func.funcCode,
              dataType: "ELEMENT",
              pageArg: p,
              pageSizeArg: ps,
            }),
        }}
      />
    );
  };

  const renderImageTable = (func: HistoryFuncRow) => {
    const key = makeRecordKey(func.deviceCode, func.funcCode, func.dataType);
    const rs = recordMap[key];
    const records = (rs?.list as Device.HistoryImage[]) || [];

    type RowType = { key: string; reportTime: string; size: number; data: string };
    const dataSource: RowType[] = records.map((r, idx) => ({
      key: `${idx}_${new Date(r.reportTime).getTime()}`,
      reportTime: new Date(r.reportTime).toLocaleString(),
      size: r.size,
      data: r.data,
    }));

    const cols: ColumnsType<RowType> = [
      { title: "时间", dataIndex: "reportTime", key: "reportTime", width: 180 },
      { title: "大小（字节）", dataIndex: "size", key: "size", width: 120 },
      {
        title: "预览",
        dataIndex: "data",
        key: "preview",
        render: (d: string) =>
          d ? <Image width={200} src={`data:image/jpeg;base64,${d}`} /> : null,
      },
    ];

    return (
      <Table<RowType>
        size="small"
        bordered
        loading={rs?.loading}
        columns={cols}
        dataSource={dataSource}
        scroll={{ x: 600 }}
        pagination={{
          current: rs?.page ?? 1,
          pageSize: rs?.pageSize ?? 10,
          total: rs?.total ?? 0,
          showSizeChanger: true,
          showTotal: (t) => `共 ${t} 条`,
          onChange: (p, ps) =>
            fetchFuncRecords({
              code: func.deviceCode,
              funcCode: func.funcCode,
              dataType: "IMAGE",
              pageArg: p,
              pageSizeArg: ps,
            }),
        }}
      />
    );
  };

  const renderHistoryFuncTable = () => {
    if (!historyDevice) return null;
    const state = funcMap[historyDevice.code];
    const funcs = state?.list ?? [];

    const dataSource: HistoryFuncRow[] = funcs.map((f) => ({
      ...f,
      deviceCode: historyDevice.code,
      key: `${historyDevice.code}_${f.funcCode}_${f.dataType}`,
    }));

    const cols: ColumnsType<HistoryFuncRow> = [
      { title: "功能码", dataIndex: "funcCode", key: "funcCode", width: 100 },
      { title: "功能名称", dataIndex: "funcName", key: "funcName", width: 160 },
      {
        title: "类型",
        dataIndex: "dataType",
        key: "dataType",
        width: 120,
        render: (dt: HistoryDataType) =>
          dt === "ELEMENT" ? <Tag color="blue">要素数据</Tag> : <Tag color="green">图片数据</Tag>,
      },
      { title: "记录数", key: "count", width: 100, render: (_, r) => r.totalRecords ?? 0 },
    ];

    return (
      <Table<HistoryFuncRow>
        size="small"
        loading={state?.loading}
        columns={cols}
        dataSource={dataSource}
        rowKey="key"
        pagination={{
          current: state?.page ?? 1,
          pageSize: state?.pageSize ?? 10,
          total: state?.total ?? 0,
          showSizeChanger: true,
          showTotal: (t) => `共 ${t} 个功能码`,
          onChange: (p, ps) => fetchFuncList(historyDevice.code, p, ps),
        }}
        expandable={{
          expandedRowRender: (record) =>
            record.dataType === "ELEMENT" ? renderElementTable(record) : renderImageTable(record),
          expandedRowKeys: expandedFuncKeys,
          onExpand: (expanded, record) => {
            setExpandedFuncKeys((prev) =>
              expanded ? [...prev, record.key] : prev.filter((k) => k !== record.key)
            );
            if (expanded)
              fetchFuncRecords({
                code: record.deviceCode,
                funcCode: record.funcCode,
                dataType: record.dataType,
              });
          },
        }}
      />
    );
  };

  // 使用 CSS Grid 布局实现等高卡片
  const wrapperStyle: React.CSSProperties = {
    display: "grid",
    gridTemplateColumns: `repeat(${columns}, 1fr)`,
    gap: 12,
  };
  // 卡片容器需要撑满 Grid 单元格高度
  const cardWrapperStyle: React.CSSProperties = { display: "flex", flexDirection: "column" };

  return (
    <PageContainer
      header={
        <div className="flex items-center justify-between">
          <h3 className="text-base font-medium m-0">设备管理</h3>
          <Space>
            <Search
              allowClear
              placeholder="设备名称 / 编码 / 类型"
              onChange={(e) => debouncedSearch(e.target.value)}
              style={{ width: 240 }}
            />
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
      <Flex gap={12} style={{ marginBottom: 16 }} wrap="nowrap">
        <Card size="small" style={{ flex: 1 }} styles={{ body: { padding: "12px 16px" } }}>
          <Flex justify="space-between" align="center" style={{ marginBottom: 10 }}>
            <span style={{ color: "#666", fontSize: 13 }}>设备总数</span>
            <span style={{ fontSize: 18, fontWeight: 600, color: "#1890ff" }}>{stats.total}</span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {Object.entries(stats.byProtocol).map(([protocol, data]) => (
              <Tag
                key={protocol}
                color="blue"
                style={{ margin: 0, padding: "4px 12px", fontSize: 14, lineHeight: "20px" }}
              >
                {protocol}: {data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card size="small" style={{ flex: 1 }} styles={{ body: { padding: "12px 16px" } }}>
          <Flex justify="space-between" align="center" style={{ marginBottom: 10 }}>
            <span style={{ color: "#666", fontSize: 13 }}>在线设备</span>
            <span style={{ fontSize: 18, fontWeight: 600, color: "#52c41a" }}>
              {stats.online}
              <span style={{ fontSize: 13, color: "#999", fontWeight: 400 }}> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {Object.entries(stats.byProtocol).map(([protocol, data]) => (
              <Tag
                key={protocol}
                color="green"
                style={{ margin: 0, padding: "4px 12px", fontSize: 14, lineHeight: "20px" }}
              >
                {protocol}: {data.online}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card size="small" style={{ flex: 1 }} styles={{ body: { padding: "12px 16px" } }}>
          <Flex justify="space-between" align="center" style={{ marginBottom: 10 }}>
            <span style={{ color: "#666", fontSize: 13 }}>离线设备</span>
            <span style={{ fontSize: 18, fontWeight: 600, color: "#ff4d4f" }}>
              {stats.offline}
              <span style={{ fontSize: 13, color: "#999", fontWeight: 400 }}> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {Object.entries(stats.byProtocol).map(([protocol, data]) => (
              <Tag
                key={protocol}
                color="red"
                style={{ margin: 0, padding: "4px 12px", fontSize: 14, lineHeight: "20px" }}
              >
                {protocol}: {data.offline}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
        <Card size="small" style={{ flex: 1 }} styles={{ body: { padding: "12px 16px" } }}>
          <Flex justify="space-between" align="center" style={{ marginBottom: 10 }}>
            <span style={{ color: "#666", fontSize: 13 }}>已启用</span>
            <span style={{ fontSize: 18, fontWeight: 600, color: "#722ed1" }}>
              {stats.enabled}
              <span style={{ fontSize: 13, color: "#999", fontWeight: 400 }}> / {stats.total}</span>
            </span>
          </Flex>
          <Flex gap={6} wrap="wrap">
            {Object.entries(stats.byProtocol).map(([protocol, data]) => (
              <Tag
                key={protocol}
                color="purple"
                style={{ margin: 0, padding: "4px 12px", fontSize: 14, lineHeight: "20px" }}
              >
                {protocol}: {data.enabled}/{data.total}
              </Tag>
            ))}
          </Flex>
        </Card>
      </Flex>

      <div style={wrapperStyle}>
        {isLoading && filteredDeviceList.length === 0
          ? renderSkeletons()
          : filteredDeviceList.map((device) => {
              const online = isOnline(
                device.lastHeartbeatTime,
                device.reportTime,
                device.online_timeout
              );
              const canRemoteControl = device.remote_control !== false; // 默认允许
              const downFuncs = device.downFuncs || [];
              const imageFuncs = device.imageFuncs || [];
              // 检查是否有图片数据
              const hasImageData = imageFuncs.some((f) => f.latestImage?.data);
              const downMenuItems: MenuProps["items"] = downFuncs.map((f) => ({
                key: f.funcCode,
                label: f.name,
              }));
              const imageMenuItems: MenuProps["items"] = imageFuncs.map((f) => ({
                key: f.funcCode,
                label: f.name,
              }));
              const isThisCardPopoverOpen =
                commandPopoverOpen && commandDevice?.code === device.code;

              return (
                <div key={device.code} style={cardWrapperStyle}>
                  <DeviceCard
                    title={
                      <Flex justify="space-between" style={{ width: "100%" }}>
                        <span>
                          {device.deviceName}:{device.code}
                        </span>
                        {online ? <Tag color="success">在线</Tag> : <Tag color="error">离线</Tag>}
                      </Flex>
                    }
                    subtitle={
                      <Flex justify="space-between" style={{ width: "100%" }}>
                        <span>
                          <Tag color="blue" style={{ marginRight: 4 }}>
                            {device.link_name || "未绑定链路"}
                          </Tag>
                          <Tag color="purple">{device.protocol_name || device.typeName}</Tag>
                        </span>
                        <span style={{ color: "#999", fontSize: 12 }}>
                          上报：{formatReportTime(device.reportTime)}
                        </span>
                      </Flex>
                    }
                    items={convertElements(device.elements)}
                    column={2}
                    length={20}
                    extra={
                      <Flex align="center" justify="space-around" style={{ width: "100%" }}>
                        {/* 图片查看 - 没有图片数据时禁用 */}
                        <Dropdown
                          disabled={!hasImageData}
                          menu={{
                            items: imageMenuItems,
                            onClick: ({ key }) => {
                              const func = imageFuncs.find((f) => f.funcCode === key);
                              if (func) handleImageClick(func);
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

                        <span
                          style={{
                            borderLeft: "1px solid #d9d9d9",
                            margin: "0 4px",
                            height: "14px",
                            display: "inline-block",
                          }}
                        />

                        {/* 指令下发 - 禁止远控时禁用 */}
                        <Popover
                          open={isThisCardPopoverOpen}
                          trigger="click"
                          placement="bottomRight"
                          content={renderCommandPopoverContent()}
                          onOpenChange={(open) => {
                            if (!open) setCommandPopoverOpen(false);
                          }}
                        >
                          <Dropdown
                            disabled={!downFuncs.length || !canRemoteControl}
                            menu={{
                              items: downMenuItems,
                              onClick: ({ key }) => {
                                const func = downFuncs.find((f) => f.funcCode === key);
                                if (func) openCommandPopover(device, func);
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
                                disabled={!downFuncs.length || !canRemoteControl}
                              />
                            </Tooltip>
                          </Dropdown>
                        </Popover>

                        <span
                          style={{
                            borderLeft: "1px solid #d9d9d9",
                            margin: "0 4px",
                            height: "14px",
                            display: "inline-block",
                          }}
                        />

                        {/* 历史数据 */}
                        <Tooltip title="历史数据">
                          <Button
                            type="text"
                            size="small"
                            icon={<HistoryOutlined />}
                            onClick={() => openHistoryModal(device)}
                          />
                        </Tooltip>

                        <span
                          style={{
                            borderLeft: "1px solid #d9d9d9",
                            margin: "0 4px",
                            height: "14px",
                            display: "inline-block",
                          }}
                        />

                        {/* 编辑 */}
                        {canEdit && (
                          <Tooltip title="编辑设备">
                            <Button
                              type="text"
                              size="small"
                              icon={<EditOutlined />}
                              onClick={() => openEditModal(device)}
                            />
                          </Tooltip>
                        )}

                        {/* 删除 */}
                        {canDelete && (
                          <>
                            <span
                              style={{
                                borderLeft: "1px solid #d9d9d9",
                                margin: "0 4px",
                                height: "14px",
                                display: "inline-block",
                              }}
                            />
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
            })}
      </div>

      {/* 图片预览 */}
      <ImagePreviewModal ref={imageModalRef} />

      {/* 设备表单弹窗 */}
      <Modal
        open={formModalVisible}
        title={editing ? "编辑设备" : "新建设备"}
        onCancel={() => {
          setFormModalVisible(false);
          setEditing(null);
          form.resetFields();
        }}
        onOk={() => form.submit()}
        confirmLoading={saveMutation.isPending}
        destroyOnHidden
        width={520}
      >
        <Form<DeviceFormValues> form={form} layout="vertical" onFinish={onFormFinish}>
          <Form.Item name="id" hidden>
            <Input />
          </Form.Item>
          <Form.Item
            label="设备名称"
            name="name"
            rules={[{ required: true, message: "请输入设备名称" }]}
          >
            <Input placeholder="设备名称" />
          </Form.Item>
          <Form.Item
            label="关联链路"
            name="link_id"
            rules={[{ required: true, message: "请选择关联链路" }]}
          >
            <Select placeholder="选择链路">
              {linkOptions.map((opt) => (
                <Select.Option key={opt.id} value={opt.id}>
                  {opt.name} ({opt.mode} - {opt.ip}:{opt.port})
                </Select.Option>
              ))}
            </Select>
          </Form.Item>
          <Form.Item
            label="协议配置"
            name="protocol_config_id"
            rules={[{ required: true, message: "请选择协议配置" }]}
          >
            <Select placeholder="选择协议配置">
              {protocolOptions?.list?.map((opt) => (
                <Select.Option key={opt.id} value={opt.id}>
                  {opt.name}
                </Select.Option>
              ))}
            </Select>
          </Form.Item>
          <Form.Item
            label="设备编码"
            name="device_code"
            rules={[
              { required: true, message: "请输入设备编码" },
              { pattern: /^[A-Za-z0-9]+$/, message: "设备编码只能包含字母和数字" },
            ]}
            extra="遥测站地址，用于协议通信识别"
          >
            <Input placeholder="如: 12345678" />
          </Form.Item>
          <Form.Item label="状态" name="status" rules={[{ required: true }]}>
            <Select>
              <Select.Option value="enabled">启用</Select.Option>
              <Select.Option value="disabled">禁用</Select.Option>
            </Select>
          </Form.Item>
          <Form.Item
            label="在线超时时间"
            name="online_timeout"
            extra="设备无心跳或数据上报超过此时间视为离线，单位：秒"
          >
            <InputNumber placeholder="默认 300 秒（5分钟）" min={1} style={{ width: "100%" }} />
          </Form.Item>
          <Form.Item
            label="允许远控"
            name="remote_control"
            valuePropName="checked"
            extra="关闭后将禁止对该设备下发指令"
          >
            <Switch checkedChildren="是" unCheckedChildren="否" />
          </Form.Item>
          <Form.Item label="备注" name="remark">
            <Input.TextArea rows={3} placeholder="备注信息" />
          </Form.Item>
        </Form>
      </Modal>

      {/* 历史数据弹窗 */}
      <Modal
        open={historyModalVisible}
        title={`历史数据 - ${historyDevice?.deviceName || ""}`}
        onCancel={() => {
          setHistoryModalVisible(false);
          setHistoryDevice(null);
        }}
        footer={null}
        width="80%"
        destroyOnHidden
        styles={{
          body: { height: "70vh", display: "flex", flexDirection: "column", overflow: "hidden" },
        }}
      >
        <Form
          form={historyForm}
          layout="inline"
          style={{ marginBottom: 16, flexShrink: 0 }}
          onFinish={() => historyDevice && fetchFuncList(historyDevice.code, 1)}
        >
          <Form.Item
            label="时间范围"
            name="timeRange"
            rules={[{ required: true, message: "请选择时间范围" }]}
          >
            <RangePicker showTime style={{ width: 340 }} />
          </Form.Item>
          <Form.Item>
            <Space>
              <Button type="primary" htmlType="submit">
                查询
              </Button>
              <Button
                onClick={() => {
                  historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
                  if (historyDevice) {
                    fetchFuncList(historyDevice.code, 1);
                  }
                }}
              >
                重置
              </Button>
            </Space>
          </Form.Item>
        </Form>
        <div style={{ flex: 1, overflow: "auto" }}>{renderHistoryFuncTable()}</div>
      </Modal>
    </PageContainer>
  );
};

export default DevicePage;
