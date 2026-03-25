/**
 * S7 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
 */

import { DownloadOutlined, UploadOutlined } from "@ant-design/icons";
import {
  Button,
  Card,
  Empty,
  Flex,
  Form,
  Input,
  InputNumber,
  Modal,
  Row,
  Popconfirm,
  Result,
  Select,
  Skeleton,
  Space,
  Col,
  Switch,
  Table,
  Tag,
  Tooltip,
  Tree,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useMemo, useState, type CSSProperties } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission, useProtocolImportExport } from "@/hooks";
import { useProtocolConfigDelete, useProtocolConfigList, useProtocolConfigSave } from "@/services";
import type { S7 } from "@/types";

type DeviceTypeFormValues = {
  deviceType: string;
  plcModel: S7.PlcModel;
  connectionMode: S7.ConnectionMode;
  connectionType: S7.ConnectionType;
  rack: number;
  slot: number;
  localTSAP: string;
  remoteTSAP: string;
  pollInterval: number;
  enabled: boolean;
  remark?: string;
};

type PlcConnectionPreset = {
  value: S7.PlcModel;
  label: string;
  mode: S7.ConnectionMode;
  rack: number;
  slot: number;
  localTSAP: string;
  remoteTSAP: string;
};

/** 生成唯一 ID（兼容非安全上下文） */
const generateId = (): string =>
  "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
    (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
  );

const defaultConfig = (): S7.Config => ({
  deviceType: "",
  plcModel: "S7-1200",
  connection: {
    mode: "RACK_SLOT",
    rack: 0,
    slot: 1,
    connectionType: "PG",
  },
  pollInterval: 5,
  areas: [],
});

const plcModelOptions: PlcConnectionPreset[] = [
  {
    value: "S7-200",
    label: "S7-200",
    mode: "TSAP",
    rack: 0,
    slot: 1,
    localTSAP: "4D57",
    remoteTSAP: "4D57",
  },
  {
    value: "S7-300",
    label: "S7-300",
    mode: "RACK_SLOT",
    rack: 0,
    slot: 2,
    localTSAP: "0100",
    remoteTSAP: "0102",
  },
  {
    value: "S7-400",
    label: "S7-400",
    mode: "RACK_SLOT",
    rack: 0,
    slot: 3,
    localTSAP: "0100",
    remoteTSAP: "0103",
  },
  {
    value: "S7-1200",
    label: "S7-1200",
    mode: "RACK_SLOT",
    rack: 0,
    slot: 1,
    localTSAP: "0100",
    remoteTSAP: "0101",
  },
  {
    value: "S7-1500",
    label: "S7-1500",
    mode: "RACK_SLOT",
    rack: 0,
    slot: 1,
    localTSAP: "0100",
    remoteTSAP: "0101",
  },
];

const getPlcPreset = (plcModel: S7.PlcModel) =>
  plcModelOptions.find((option) => option.value === plcModel) ??
  plcModelOptions.find((option) => option.value === "S7-1200") ??
  plcModelOptions[0];

const normalizeTsapValue = (value?: string) => {
  if (!value) return undefined;
  const normalized = value
    .replace(/^0x/i, "")
    .replace(/[\s.:\-_]/g, "")
    .toUpperCase();
  return normalized || undefined;
};

const formatTsapValue = (value?: string) => {
  const normalized = normalizeTsapValue(value);
  if (!normalized || !/^[0-9A-F]{1,4}$/.test(normalized)) {
    return undefined;
  }
  return normalized.padStart(4, "0");
};

const getConnectionTypeCode = (connectionType?: S7.ConnectionType) => {
  if (connectionType === "OP") return 0x02;
  if (connectionType === "S7_BASIC") return 0x03;
  return 0x01;
};

const buildRemoteTsapFromRackSlot = (
  rack: number,
  slot: number,
  connectionType?: S7.ConnectionType
) =>
  ((getConnectionTypeCode(connectionType) << 8) + rack * 0x20 + slot)
    .toString(16)
    .toUpperCase()
    .padStart(4, "0");

const inferConnectionMode = (
  plcModel: S7.PlcModel,
  connection?: S7.Connection
): S7.ConnectionMode => {
  if (connection?.mode === "TSAP" || connection?.mode === "RACK_SLOT") {
    return connection.mode;
  }
  if (connection?.localTSAP || connection?.remoteTSAP) {
    return "TSAP";
  }
  return getPlcPreset(plcModel).mode;
};

const getConnectionFormValues = (plcModel: S7.PlcModel, connection?: S7.Connection) => {
  const preset = getPlcPreset(plcModel);
  const connectionMode = inferConnectionMode(plcModel, connection);
  const connectionType = connection?.connectionType ?? "PG";
  const rack = connection?.rack ?? preset.rack;
  const slot = connection?.slot ?? preset.slot;
  const localTSAP =
    formatTsapValue(connection?.localTSAP) ??
    (connectionMode === "TSAP" && preset.mode === "TSAP" ? preset.localTSAP : "0100");
  const remoteTSAP =
    formatTsapValue(connection?.remoteTSAP) ??
    (connectionMode === "TSAP" && preset.mode === "TSAP"
      ? preset.remoteTSAP
      : buildRemoteTsapFromRackSlot(rack, slot, connectionType));

  return {
    connectionMode,
    connectionType,
    rack,
    slot,
    localTSAP,
    remoteTSAP,
  };
};

const buildConnectionConfig = (values: DeviceTypeFormValues): S7.Connection => {
  if (values.connectionMode === "TSAP") {
    return {
      mode: "TSAP",
      connectionType: values.connectionType,
      rack: undefined,
      slot: undefined,
      localTSAP: formatTsapValue(values.localTSAP),
      remoteTSAP: formatTsapValue(values.remoteTSAP),
    };
  }

  return {
    mode: "RACK_SLOT",
    connectionType: values.connectionType,
    rack: values.rack,
    slot: values.slot,
    localTSAP: undefined,
    remoteTSAP: undefined,
  };
};

const connectionTypeOptions: { value: S7.ConnectionType; label: string }[] = [
  { value: "PG", label: "PG" },
  { value: "OP", label: "OP" },
  { value: "S7_BASIC", label: "S7BASIC" },
];

const connectionModeOptions: { value: S7.ConnectionMode; label: string }[] = [
  { value: "RACK_SLOT", label: "Rack/Slot" },
  { value: "TSAP", label: "TSAP" },
];

const connectionTypeTips: Record<S7.ConnectionType, string> = {
  PG: "PG（编程）模式，常用于本地接线连接与离线调试",
  OP: "OP（操作）模式，适合上位机常态采集场景",
  S7_BASIC: "S7 Basic，兼容部分第三方网关的轻量模式",
};

const connectionModeTips: Record<S7.ConnectionMode, string> = {
  RACK_SLOT: "常规西门子 PLC 连接方式，适合 S7-300/400/1200/1500 等场景",
  TSAP: "直接指定本地/远端 TSAP，适合 S7-200、CP243 或需要手工指定 TSAP 的场景",
};

const getConnectionTypeLabel = (connectionType?: S7.ConnectionType) =>
  connectionTypeOptions.find((item) => item.value === connectionType)?.label || "PG";

const getConnectionModeLabel = (connectionMode?: S7.ConnectionMode) =>
  connectionModeOptions.find((item) => item.value === connectionMode)?.label || "Rack/Slot";

const normalizeAreaTypeForPlcModel = (
  plcModel?: S7.PlcModel,
  areaType?: S7.AreaType
): S7.AreaType | undefined => {
  if (!areaType) {
    return areaType;
  }
  if (plcModel === "S7-200" && areaType === "DB") {
    return "V";
  }
  return areaType;
};

const getAreaTypeOptions = (plcModel?: S7.PlcModel): { value: S7.AreaType; label: string }[] => {
  if (plcModel === "S7-200") {
    return [
      { value: "V", label: "V（变量存储器）" },
      { value: "MK", label: "M（MK，标记位）" },
      { value: "PE", label: "I（PE，系统输入）" },
      { value: "PA", label: "Q（PA，系统输出）" },
      { value: "CT", label: "C（CT，计数器）" },
      { value: "TM", label: "T（TM，定时器）" },
    ];
  }

  return [
    { value: "DB", label: "DB（数据块）" },
    { value: "MK", label: "M（MK，标记位）" },
    { value: "PE", label: "I（PE，系统输入）" },
    { value: "PA", label: "Q（PA，系统输出）" },
    { value: "CT", label: "C（CT，计数器）" },
    { value: "TM", label: "T（TM，定时器）" },
  ];
};

const validateTsapValue = async (_: unknown, value?: string) => {
  if (!formatTsapValue(value)) {
    throw new Error("请输入 1-4 位十六进制 TSAP，例如 4D57 或 0200");
  }
};

const areaDataTypeOptions: { value: S7.AreaDataType; label: string }[] = [
  { value: "BOOL", label: "布尔（BOOL）" },
  { value: "INT8", label: "8位整型（INT8）" },
  { value: "UINT8", label: "8位无符号整型（UINT8）" },
  { value: "INT16", label: "16位有符号整型（INT16）" },
  { value: "UINT16", label: "16位无符号整型（UINT16）" },
  { value: "INT32", label: "32位有符号整型（INT32）" },
  { value: "UINT32", label: "32位无符号整型（UINT32）" },
  { value: "FLOAT", label: "浮点（FLOAT）" },
  { value: "LREAL", label: "双精度（LREAL）" },
  { value: "STRING", label: "字符串（STRING）" },
];

const areaDataTypeSizeMap: Record<S7.AreaDataType, number> = {
  BOOL: 1,
  INT8: 1,
  UINT8: 1,
  INT16: 2,
  UINT16: 2,
  INT32: 4,
  UINT32: 4,
  FLOAT: 4,
  LREAL: 8,
  STRING: 1,
};

const getDataTypeSize = (dataType?: S7.AreaDataType) =>
  dataType ? (areaDataTypeSizeMap[dataType] ?? 1) : 1;

const writableAreaTypes: S7.AreaType[] = ["DB", "V", "MK", "PA"];
const bitOnlyAreaTypes: S7.AreaType[] = ["PE", "PA"];

const getAreaDataTypeOptions = (
  areaType?: S7.AreaType
): { value: S7.AreaDataType; label: string }[] => {
  if (areaType === "CT" || areaType === "TM") {
    return areaDataTypeOptions.filter((item) => item.value === "UINT16");
  }
  if (bitOnlyAreaTypes.includes(areaType || "DB")) {
    return areaDataTypeOptions.filter((item) => item.value === "BOOL");
  }
  return areaDataTypeOptions;
};

const areaAddressPrefixMap: Record<S7.AreaType, { bool: string; number: string }> = {
  DB: { bool: "X", number: "DB" },
  V: { bool: "V", number: "V" },
  MK: { bool: "M", number: "M" },
  PE: { bool: "I", number: "I" },
  PA: { bool: "Q", number: "Q" },
  CT: { bool: "C", number: "C" },
  TM: { bool: "T", number: "T" },
};

const areaAddressHintMap: Record<S7.AreaType, string> = {
  DB: "DB1.DBX0.0、DB1.DBW10、DB1.DBD0、DB1.DBD1（LREAL，DBD 为4字节）",
  V: "V0.0、VB10、VW20、VD30",
  MK: "M0.0、MB10、MW20、MD30",
  PE: "I0.0、IB10、IW20、ID30",
  PA: "Q0.0、QB10、QW20、QD30",
  CT: "C0、C1",
  TM: "T0、T1",
};

const areaAddressTypeMap: Record<S7.AreaDataType, string> = {
  BOOL: "X",
  INT8: "B",
  UINT8: "B",
  INT16: "W",
  UINT16: "W",
  INT32: "D",
  UINT32: "D",
  FLOAT: "D",
  LREAL: "D",
  STRING: "B",
};

const normalizeS7DataType = (value?: string): S7.AreaDataType => {
  if (value === "LREAL" || value === "DOUBLE") {
    return "LREAL";
  }
  if (
    value === "BOOL" ||
    value === "INT8" ||
    value === "UINT8" ||
    value === "INT16" ||
    value === "UINT16" ||
    value === "INT32" ||
    value === "UINT32" ||
    value === "FLOAT" ||
    value === "STRING"
  ) {
    return value;
  }
  return "INT16";
};

const getAreaAddressSample = (
  areaType?: S7.AreaType,
  dataType?: S7.AreaDataType,
  dbNumber?: number,
  start?: number,
  bit?: number
) => {
  if (!areaType) return "";
  const safeStart = typeof start === "number" ? start : 0;
  const safeBit = typeof bit === "number" ? bit : 0;
  const area = areaAddressPrefixMap[areaType];
  const suffix = areaAddressTypeMap[dataType || "INT16"];
  if (areaType === "CT" || areaType === "TM") {
    return `${area.number}${safeStart}`;
  }
  if (areaType === "DB") {
    if (!dbNumber) return "";
    if (suffix === "X") return `DB${dbNumber}.DBX${safeStart}.${safeBit}`;
    return `DB${dbNumber}.DB${suffix}${safeStart}`;
  }
  if (areaType === "V") {
    if (suffix === "X") return `V${safeStart}.${safeBit}`;
    return `V${suffix}${safeStart}`;
  }
  if (suffix === "X") return `${area.bool}${safeStart}.${safeBit}`;
  return `${area.number}${suffix}${safeStart}`;
};

const getAddressSuffixExample = (areaType?: S7.AreaType, dataType?: S7.AreaDataType) => {
  if (!areaType) return "";
  const area = areaAddressPrefixMap[areaType];
  if (areaType === "CT" || areaType === "TM") return "";
  const suffix = areaAddressTypeMap[dataType || "INT16"];
  if (areaType === "V") {
    if (suffix === "X") return "V0.0";
    return `V${suffix}0`;
  }
  if (suffix === "X") return `${area.bool}X.0`;
  return `${area.number}${suffix}0`;
};

const supportsBitAddress = (areaType?: S7.AreaType, dataType?: S7.AreaDataType) =>
  dataType === "BOOL" && !!areaType && areaType !== "CT" && areaType !== "TM";

const getAddressRuleText = (areaType: S7.AreaType | undefined, isBool: boolean) => {
  if (areaType === "DB") {
    return isBool ? "请输入 DB 位号（如 DB1.DBX0.0）" : "请输入 DB 字节偏移（如 DB1.DBW10）";
  }
  if (areaType === "V") {
    return isBool ? "请输入 V 位地址（如 V0.0）" : "请输入 V 偏移（如 VB10 / VW20）";
  }
  if (areaType === "CT") {
    return "请输入计数器地址（如 C0）";
  }
  if (areaType === "TM") {
    return "请输入定时器地址（如 T0）";
  }
  return isBool ? "请输入位地址（如 M0.0/I0.1/Q1.2）" : "请输入字节偏移";
};

const getAreaAddressRangeText = (area: S7.Area) => {
  const dataType = normalizeS7DataType(area.dataType);
  const start = typeof area.start === "number" && area.start >= 0 ? area.start : 0;
  const size =
    typeof area.size === "number" && area.size > 0 ? area.size : getDataTypeSize(dataType) || 1;
  const end =
    dataType === "STRING"
      ? start + Math.max(size, 1) - 1
      : dataType === "LREAL"
        ? start + 1
        : start;
  const startBit = typeof area.startBit === "number" ? area.startBit : 0;

  const startAddress = getAreaAddressSample(area.area, dataType, area.dbNumber, start, startBit);
  const endAddress = getAreaAddressSample(area.area, dataType, area.dbNumber, end, startBit);

  return {
    start: startAddress || "地址异常",
    end: endAddress || "地址异常",
  };
};

interface AreaModalProps {
  open: boolean;
  mode: "create" | "edit";
  initialValue?: S7.Area;
  plcModel?: S7.PlcModel;
  onCancel: () => void;
  onSubmit: (value: S7.Area) => void;
}

function AreaModal({ open, mode, initialValue, plcModel, onCancel, onSubmit }: AreaModalProps) {
  const [form] = Form.useForm<S7.Area>();
  const areaType = Form.useWatch("area", form);
  const dataType = normalizeS7DataType(Form.useWatch("dataType", form) as string | undefined);
  const dbNumber = Form.useWatch("dbNumber", form);
  const startAddress = Form.useWatch("start", form);
  const startBit = Form.useWatch("startBit", form);
  const stringLength = Form.useWatch("size", form);
  const parsedAreaType = areaType as S7.AreaType | undefined;
  const areaTypeOptions = getAreaTypeOptions(plcModel);
  const defaultAreaType: S7.AreaType = plcModel === "S7-200" ? "V" : "DB";
  const normalizedInitialArea =
    normalizeAreaTypeForPlcModel(plcModel, initialValue?.area) ?? defaultAreaType;
  const initialDbNumber =
    normalizedInitialArea === "V"
      ? undefined
      : initialValue?.dbNumber ?? (normalizedInitialArea === "DB" ? 1 : undefined);
  const initialFormValues = initialValue
    ? {
        ...initialValue,
        area: normalizedInitialArea,
        dbNumber: initialDbNumber,
      }
    : {
        name: "",
        area: defaultAreaType,
        dbNumber: initialDbNumber,
        dataType: "INT16",
        start: 0,
        size: 1,
        writable: false,
        remark: "",
        startBit: undefined,
      };
  const isStringType = dataType === "STRING";
  const isBoolDataType = dataType === "BOOL";
  const isWritableArea = writableAreaTypes.includes(parsedAreaType as S7.AreaType);
  const showBitInput = supportsBitAddress(parsedAreaType, dataType);
  const addressExample = getAddressSuffixExample(parsedAreaType, dataType);
  const calcPreviewSize =
    dataType === "STRING"
      ? typeof stringLength === "number" && stringLength > 0
        ? stringLength
        : 1
      : getDataTypeSize(dataType);
  const startOffset = typeof startAddress === "number" && startAddress >= 0 ? startAddress : 0;
  const endOffset =
    dataType === "STRING"
      ? startOffset + Math.max(calcPreviewSize, 1) - 1
      : dataType === "LREAL"
        ? startOffset + 1
        : startOffset;
  const addressSample = getAreaAddressSample(
    parsedAreaType,
    dataType,
    dbNumber,
    startOffset,
    startBit
  );
  const endAddressSample = getAreaAddressSample(
    parsedAreaType,
    dataType,
    dbNumber,
    endOffset,
    startBit
  );
  const canUseBoolAddress =
    parsedAreaType === "DB" ||
    parsedAreaType === "V" ||
    parsedAreaType === "MK" ||
    parsedAreaType === "PE" ||
    parsedAreaType === "PA";

  const handleOk = async () => {
    const values = await form.validateFields();
    const resolvedDataType =
      parsedAreaType === "CT" || parsedAreaType === "TM"
        ? values.dataType || "UINT16"
        : parsedAreaType === "PE" || parsedAreaType === "PA"
          ? "BOOL"
          : values.dataType;
    const size = resolvedDataType === "STRING" ? values.size : getDataTypeSize(resolvedDataType);
    const nextDbNumber = parsedAreaType === "DB" ? values.dbNumber : undefined;
    const nextStartBit = isBoolDataType && canUseBoolAddress ? values.startBit : undefined;
    const nextValues = {
      ...values,
      dbNumber: nextDbNumber,
      startBit: nextStartBit,
      dataType: resolvedDataType,
      id: mode === "create" ? generateId() : initialValue?.id || values.id,
      size,
      writable: isWritableArea ? values.writable : false,
    };
    onSubmit(nextValues);
  };

  return (
    <Modal
      title={mode === "create" ? "新增寄存器" : "编辑寄存器"}
      open={open}
      onCancel={onCancel}
      onOk={handleOk}
      destroyOnHidden
    >
      <Form
        form={form}
        layout="vertical"
        initialValues={initialFormValues}
      >
        <Form.Item
          name="name"
          label="寄存器名称"
          rules={[{ required: true, message: "请输入寄存器名称" }]}
        >
          <Input placeholder="例如: 温度寄存器" />
        </Form.Item>
        <Row gutter={12}>
          <Col xs={24} sm={12}>
            <Form.Item
              name="area"
              label="寄存器类型"
              rules={[{ required: true }]}
              extra={plcModel === "S7-200" ? "S7-200 使用 V/M/I/Q/C/T 区域" : "不同区域类型映射不同读取方式"}
            >
              <Select
                options={areaTypeOptions}
                onChange={(value: S7.AreaType) => {
                  const currentDataType = normalizeS7DataType(
                    form.getFieldValue("dataType") as string | undefined
                  );
                  const updates: Partial<S7.Area> = {};
                  const normalizedDataType =
                    value === "CT" || value === "TM" ? "UINT16" : currentDataType;

                  if (value === "CT" || value === "TM") {
                    updates.dataType = normalizedDataType;
                    updates.size = getDataTypeSize(normalizedDataType);
                  } else if (bitOnlyAreaTypes.includes(value)) {
                    updates.dataType = "BOOL";
                    updates.size = getDataTypeSize("BOOL");
                  } else if (normalizedDataType !== "STRING") {
                    updates.size = getDataTypeSize(normalizedDataType);
                  }

                  if (value !== "DB") {
                    updates.dbNumber = undefined;
                  }
                  if (value === "DB" && form.getFieldValue("dbNumber") == null) {
                    updates.dbNumber = 1;
                  }
                  updates.startBit = undefined;

                  if (Object.keys(updates).length > 0) {
                    form.setFieldsValue(updates);
                  }
                }}
              />
            </Form.Item>
          </Col>
          <Col xs={24} sm={12}>
            <Form.Item
              name="dataType"
              label="数据类型"
              rules={[{ required: true }]}
              extra="不同数据类型对应不同解析方式"
            >
              <Select
                options={getAreaDataTypeOptions(areaType as S7.AreaType | undefined)}
                onChange={(value: S7.AreaDataType) => {
                  const updates: Partial<S7.Area> = {};
                  if (value !== "BOOL") {
                    updates.startBit = undefined;
                  }
                  if (value === "STRING") {
                    const currentSize = form.getFieldValue("size") as number | undefined;
                    if (typeof currentSize !== "number" || Number.isNaN(currentSize)) {
                      updates.size = 1;
                    }
                  } else {
                    updates.size = getDataTypeSize(value);
                  }
                  if (Object.keys(updates).length > 0) {
                    form.setFieldsValue(updates);
                  }
                }}
              />
            </Form.Item>
          </Col>
        </Row>
        {areaType === "DB" && (
          <Row gutter={12}>
            <Col xs={24} sm={12}>
              <Form.Item
                name="dbNumber"
                label="DB 编号"
                rules={[{ required: true, message: "请输入 DB 编号" }]}
                extra="示例：S7-300 上可读 DB1~DB999；S7-1200 常见为 DB1、DB100 等"
              >
                <InputNumber min={1} className="w-full" />
              </Form.Item>
            </Col>
          </Row>
        )}
        <Flex gap={16} className="w-full" align="flex-start" wrap>
          <Form.Item
            name="start"
            label="起始偏移"
            rules={[
              { required: true, message: getAddressRuleText(parsedAreaType, isBoolDataType) },
              { type: "number", min: 0, message: "偏移不能小于 0" },
            ]}
            className="flex-1"
          >
            <InputNumber min={0} className="!w-full" />
          </Form.Item>
          {showBitInput && (
            <Form.Item
              name="startBit"
              label="位号"
              rules={
                isBoolDataType
                  ? [
                      { required: true, message: "请输入位号" },
                      { type: "number", min: 0, max: 7, message: "位号只能是 0~7" },
                    ]
                  : undefined
              }
              className="flex-1"
            >
              <InputNumber min={0} max={7} className="!w-full" />
            </Form.Item>
          )}
          {isStringType && (
            <Form.Item
              name="size"
              label="长度（字节）"
              rules={[{ required: true, message: "请输入字符串长度" }]}
              className="flex-1"
              extra="字符串长度单位为字节"
            >
              <InputNumber min={1} className="!w-full" />
            </Form.Item>
          )}
        </Flex>
        <Form.Item label="地址示例">
          <div className="text-xs text-gray-500">
            参考示例：
            {addressExample || (parsedAreaType ? "按区域规则自动拼接" : "请先选择寄存器类型")}
          </div>
          <div className="text-xs text-gray-500">
            {areaType ? `示例：${areaAddressHintMap[areaType]}` : "请先选择寄存器类型"}
          </div>
          <div className="text-xs text-gray-500">当前起始地址：{addressSample || "暂无"}</div>
          <div className="text-xs text-gray-500">当前结束地址：{endAddressSample || "暂无"}</div>
        </Form.Item>
        {isWritableArea && (
          <Form.Item name="writable" label="可写" valuePropName="checked">
            <Switch />
          </Form.Item>
        )}
        <Form.Item name="remark" label="备注">
          <Input.TextArea rows={3} placeholder="备注说明" />
        </Form.Item>
      </Form>
    </Modal>
  );
}

const S7ConfigPage = () => {
  const canQuery = usePermission("iot:protocol:query");
  const canAdd = usePermission("iot:protocol:add");
  const canEdit = usePermission("iot:protocol:edit");
  const canDelete = usePermission("iot:protocol:delete");
  const canImport = usePermission("iot:protocol:import");
  const canExport = usePermission("iot:protocol:export");

  const {
    data: configPage,
    isLoading: loadingTypes,
    refetch,
  } = useProtocolConfigList({ protocol: "S7" }, { enabled: canQuery });
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();
  const { exportConfigs, triggerImport, importing } = useProtocolImportExport("S7");

  const [selectedTypeId, setSelectedTypeId] = useState<number>();
  const [deviceTypeModalOpen, setDeviceTypeModalOpen] = useState(false);
  const [editingDeviceType, setEditingDeviceType] = useState<boolean>(false);
  const [createForm] = Form.useForm<DeviceTypeFormValues>();
  const [areaModalOpen, setAreaModalOpen] = useState(false);
  const [editingAreaId, setEditingAreaId] = useState<string | null>(null);

  const types = useMemo(() => configPage?.list || [], [configPage?.list]);
  const emptyTypeDesc = types.length ? "未选择设备类型" : "暂无设备类型";
  const selectedConnectionType = Form.useWatch("connectionType", createForm) as
    | S7.ConnectionType
    | undefined;
  const selectedConnectionMode = Form.useWatch("connectionMode", createForm) as
    | S7.ConnectionMode
    | undefined;
  const connectionTypeTip = connectionTypeTips[selectedConnectionType || "PG"];
  const connectionModeTip = connectionModeTips[selectedConnectionMode || "RACK_SLOT"];

  const activeTypeId = useMemo(() => {
    if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
      return selectedTypeId;
    }
    return types.length > 0 ? types[0].id : undefined;
  }, [selectedTypeId, types]);

  const activeType = useMemo(() => types.find((t) => t.id === activeTypeId), [activeTypeId, types]);

  const activeConfig = (activeType?.config as S7.Config) ?? null;
  const activeAreas = activeConfig?.areas ?? [];
  const activeConnectionMode = activeConfig
    ? inferConnectionMode(activeConfig.plcModel, activeConfig.connection)
    : undefined;
  const activeConnectionPreset = activeConfig
    ? getConnectionFormValues(activeConfig.plcModel, activeConfig.connection)
    : null;

  const areaColumns: ColumnsType<S7.Area> = [
    { title: "寄存器名称", dataIndex: "name", ellipsis: true },
    {
      title: "寄存器类型",
      dataIndex: "area",
      render: (value: S7.AreaType) => {
        const displayArea = normalizeAreaTypeForPlcModel(activeConfig?.plcModel, value) ?? value;
        return <Tag color="blue">{displayArea}</Tag>;
      },
    },
    {
      title: "数据类型",
      dataIndex: "dataType",
      ellipsis: true,
      render: (value?: string) => (value ? normalizeS7DataType(value) : "-"),
    },
    {
      title: "DB 编号",
      dataIndex: "dbNumber",
      render: (value: number | undefined, record: S7.Area) => {
        const displayArea = normalizeAreaTypeForPlcModel(activeConfig?.plcModel, record.area) ?? record.area;
        if (displayArea === "V") {
          return "-";
        }
        return value ?? "-";
      },
    },
    {
      title: "起始地址",
      render: (_, record: S7.Area) => {
        const displayArea = normalizeAreaTypeForPlcModel(activeConfig?.plcModel, record.area) ?? record.area;
        return getAreaAddressRangeText({ ...record, area: displayArea }).start;
      },
    },
    {
      title: "结束地址",
      render: (_, record: S7.Area) => {
        const displayArea = normalizeAreaTypeForPlcModel(activeConfig?.plcModel, record.area) ?? record.area;
        return getAreaAddressRangeText({ ...record, area: displayArea }).end;
      },
    },
    { title: "长度（字节）", dataIndex: "size" },
    {
      title: "可写",
      dataIndex: "writable",
      render: (value?: boolean) => (value ? <Tag color="orange">可写</Tag> : <Tag>只读</Tag>),
    },
    { title: "备注", dataIndex: "remark", ellipsis: true },
    {
      title: "操作",
      width: 160,
      render: (_, record) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => {
                setEditingAreaId(record.id);
                setAreaModalOpen(true);
              }}
            >
              编辑
            </Button>
          )}
          {canDelete && (
            <Popconfirm
              title="确认删除该寄存器？"
              onConfirm={async () => {
                if (!activeTypeId || !activeConfig) return;
                const nextAreas = activeConfig.areas.filter((item) => item.id !== record.id);
                await saveMutation.mutateAsync({
                  id: activeTypeId,
                  protocol: "S7",
                  config: {
                    ...activeConfig,
                    areas: nextAreas,
                  },
                });
                await refetch();
              }}
            >
              <Button size="small" danger type="link">
                删除
              </Button>
            </Popconfirm>
          )}
        </Space>
      ),
    },
  ];

  const handleDeleteType = async () => {
    if (!activeTypeId) return;
    const idx = types.findIndex((t) => t.id === activeTypeId);
    const nextType = types[idx + 1] ?? types[idx - 1];
    await deleteMutation.mutateAsync(activeTypeId);
    setSelectedTypeId(nextType?.id);
    await refetch();
  };

  const handleOpenCreateType = () => {
    setEditingDeviceType(false);
    setDeviceTypeModalOpen(true);
    createForm.setFieldsValue({
      deviceType: "",
      plcModel: "S7-1200",
      ...getConnectionFormValues("S7-1200"),
      pollInterval: 5,
      enabled: true,
      remark: "",
    });
  };

  const handleOpenEditType = () => {
    if (!activeType) return;
    const currentConfig = (activeType.config as S7.Config) ?? defaultConfig();
    setEditingDeviceType(true);
    setDeviceTypeModalOpen(true);
    createForm.setFieldsValue({
      deviceType: activeType.name,
      plcModel: currentConfig.plcModel ?? "S7-1200",
      ...getConnectionFormValues(currentConfig.plcModel ?? "S7-1200", currentConfig.connection),
      pollInterval: currentConfig.pollInterval ?? 5,
      enabled: activeType.enabled,
      remark: activeType.remark,
    });
  };

  const handlePlcModelChange = (plcModel: S7.PlcModel) => {
    const preset = getPlcPreset(plcModel);
    createForm.setFieldsValue({
      connectionMode: preset.mode,
      rack: preset.rack,
      slot: preset.slot,
      localTSAP: preset.localTSAP,
      remoteTSAP: preset.remoteTSAP,
    });
  };

  const handleConnectionModeChange = (connectionMode: S7.ConnectionMode) => {
    const plcModel = (createForm.getFieldValue("plcModel") as S7.PlcModel | undefined) ?? "S7-1200";
    const preset = getPlcPreset(plcModel);
    if (connectionMode === "RACK_SLOT") {
      createForm.setFieldsValue({
        rack: (createForm.getFieldValue("rack") as number | undefined) ?? preset.rack,
        slot: (createForm.getFieldValue("slot") as number | undefined) ?? preset.slot,
      });
      return;
    }

    const connectionType =
      (createForm.getFieldValue("connectionType") as S7.ConnectionType | undefined) ?? "PG";
    const rack = (createForm.getFieldValue("rack") as number | undefined) ?? preset.rack;
    const slot = (createForm.getFieldValue("slot") as number | undefined) ?? preset.slot;
    const currentLocalTSAP = formatTsapValue(
      createForm.getFieldValue("localTSAP") as string | undefined
    );
    const currentRemoteTSAP = formatTsapValue(
      createForm.getFieldValue("remoteTSAP") as string | undefined
    );

    createForm.setFieldsValue({
      localTSAP: currentLocalTSAP ?? (preset.mode === "TSAP" ? preset.localTSAP : "0100"),
      remoteTSAP:
        currentRemoteTSAP ??
        (preset.mode === "TSAP"
          ? preset.remoteTSAP
          : buildRemoteTsapFromRackSlot(rack, slot, connectionType)),
    });
  };

  const handleSaveDeviceType = async () => {
    const values = await createForm.validateFields();
    if (editingDeviceType) {
      if (!activeTypeId || !activeType) return;
      const currentConfig = (activeType.config as S7.Config) ?? defaultConfig();
      const nextConfig: S7.Config = {
        ...currentConfig,
        plcModel: values.plcModel,
        connection: {
          ...currentConfig.connection,
          ...buildConnectionConfig(values),
        },
        pollInterval: values.pollInterval,
      };
      await saveMutation.mutateAsync({
        id: activeTypeId,
        protocol: "S7",
        name: values.deviceType,
        enabled: values.enabled,
        config: nextConfig,
        remark: values.remark,
      });
    } else {
      const config = defaultConfig();
      const nextConfig: S7.Config = {
        ...config,
        deviceType: values.deviceType,
        plcModel: values.plcModel,
        pollInterval: values.pollInterval,
        connection: buildConnectionConfig(values),
      };
      await saveMutation.mutateAsync({
        protocol: "S7",
        name: values.deviceType,
        enabled: values.enabled,
        config: nextConfig,
        remark: values.remark,
      });
    }
    setDeviceTypeModalOpen(false);
    setEditingDeviceType(false);
    createForm.resetFields();
    await refetch();
  };

  const loadingAreas = loadingTypes;
  const editingArea = activeConfig?.areas.find((area) => area.id === editingAreaId);

  if (!canQuery) {
    return (
      <PageContainer title="S7配置">
        <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
      </PageContainer>
    );
  }

  return (
    <PageContainer title="S7配置">
      <div className="h-full flex">
        <div className="w-[360px] shrink-0 pr-3 h-full">
          <Card
            title="设备类型"
            className="h-full flex flex-col"
            styles={{ body: { flex: 1, overflow: "auto", padding: 16 } }}
            extra={
              <Space size={4}>
                {canAdd && (
                  <Button size="small" type="primary" onClick={handleOpenCreateType}>
                    新增
                  </Button>
                )}
                {canEdit && (
                  <Button size="small" disabled={!activeTypeId} onClick={handleOpenEditType}>
                    编辑
                  </Button>
                )}
                {canDelete && (
                  <Popconfirm
                    title="确认删除该设备类型？"
                    onConfirm={handleDeleteType}
                    disabled={!activeTypeId}
                  >
                    <Button size="small" danger disabled={!activeTypeId}>
                      删除
                    </Button>
                  </Popconfirm>
                )}
                {canExport && (
                  <Tooltip title="导出">
                    <Button
                      size="small"
                      icon={<DownloadOutlined />}
                      disabled={!types.length}
                      onClick={() => exportConfigs(types)}
                    />
                  </Tooltip>
                )}
                {canImport && (
                  <Tooltip title="导入">
                    <Button
                      size="small"
                      icon={<UploadOutlined />}
                      loading={importing}
                      onClick={triggerImport}
                    />
                  </Tooltip>
                )}
              </Space>
            }
          >
            {loadingTypes ? (
              <Skeleton active paragraph={{ rows: 6 }} />
            ) : types.length === 0 ? (
              <Empty description="暂无设备类型" />
            ) : (
              <Tree
                blockNode
                className="[&_.ant-tree-switcher]:hidden"
                selectedKeys={activeTypeId ? [String(activeTypeId)] : []}
                onSelect={(keys) => {
                  if (keys.length > 0) {
                    setSelectedTypeId(Number(keys[0]));
                  }
                }}
                treeData={types.map((t) => {
                  const config = t.config as S7.Config;
                  const regCount = config?.areas?.length || 0;
                  return {
                    key: String(t.id),
                    title: (
                      <Tooltip title={t.remark || "暂无备注"} placement="right">
                        <Flex justify="space-between" align="center" className="h-8 p-1">
                          <Space size={4}>
                            <span>{t.name}</span>
                            <Tag color="blue">{regCount}个寄存器</Tag>
                          </Space>
                          {t.enabled ? <Tag color="green">启用</Tag> : <Tag color="red">禁用</Tag>}
                        </Flex>
                      </Tooltip>
                    ),
                  };
                })}
              />
            )}
          </Card>
        </div>

        <div className="flex-1 min-w-0 h-full">
          <Card
            title={
              activeType ? (
                <Space>
                  <span>寄存器配置</span>
                  {activeType?.enabled ? (
                    <Tag color="green">启用</Tag>
                  ) : (
                    <Tag color="red">禁用</Tag>
                  )}
                  <Tag color={activeConnectionMode === "TSAP" ? "cyan" : "blue"}>
                    {getConnectionModeLabel(activeConnectionMode)}
                  </Tag>
                  {activeConnectionMode === "TSAP" ? (
                    <Tag>
                      TSAP {activeConnectionPreset?.localTSAP ?? "0100"} /{" "}
                      {activeConnectionPreset?.remoteTSAP ?? "0100"}
                    </Tag>
                  ) : (
                    <>
                      <Tag color="blue">
                        {getConnectionTypeLabel(
                          (activeType.config as S7.Config)?.connection?.connectionType
                        )}
                      </Tag>
                      <Tag>
                        Rack {activeConnectionPreset?.rack ?? 0} / Slot{" "}
                        {activeConnectionPreset?.slot ?? 1}
                      </Tag>
                    </>
                  )}
                  <Tag>读取间隔 {(activeType.config as S7.Config)?.pollInterval ?? 5}s</Tag>
                </Space>
              ) : types.length > 0 ? (
                "请选择设备类型"
              ) : (
                "暂无设备类型"
              )
            }
            className="h-full flex flex-col"
            styles={{ body: { flex: 1, overflow: "auto", padding: 0 } }}
            extra={
              canAdd &&
              activeTypeId && (
                <Button
                  size="small"
                  type="primary"
                  onClick={() => {
                    setEditingAreaId(null);
                    setAreaModalOpen(true);
                  }}
                >
                  新增寄存器
                </Button>
              )
            }
          >
            {!activeType ? (
              <Empty description={emptyTypeDesc} />
            ) : (
              <Space direction="vertical" className="w-full p-4" size="large">
                <div style={{ "--ant-table-header-border-radius": 0 } as CSSProperties}>
                  <Table
                    size="small"
                    rowKey="id"
                    pagination={false}
                    loading={loadingAreas}
                    dataSource={activeAreas}
                    columns={areaColumns}
                    locale={{ emptyText: <Empty description="暂无寄存器" /> }}
                    sticky
                    scroll={{ x: "max-content" }}
                  />
                </div>
              </Space>
            )}
          </Card>
        </div>
      </div>

      <Modal
        title={editingDeviceType ? "编辑设备类型" : "新建设备类型"}
        open={deviceTypeModalOpen}
        confirmLoading={saveMutation.isPending}
        onCancel={() => {
          setDeviceTypeModalOpen(false);
          setEditingDeviceType(false);
          createForm.resetFields();
        }}
        onOk={handleSaveDeviceType}
        destroyOnHidden
      >
        <Form
          form={createForm}
          layout="vertical"
          initialValues={{
            deviceType: "",
            plcModel: "S7-1200",
            connectionMode: "RACK_SLOT",
            connectionType: "PG",
            rack: 0,
            slot: 1,
            localTSAP: "0100",
            remoteTSAP: "0101",
            pollInterval: 5,
            enabled: true,
            remark: "",
          }}
        >
          <Form.Item
            name="deviceType"
            label="名称"
            rules={[{ required: true, message: "请输入名称" }]}
            extra="用于区分同一协议下不同设备类别"
          >
            <Input placeholder="例如: 温湿度传感器" />
          </Form.Item>
          <Form.Item
            name="plcModel"
            label="PLC型号"
            rules={[{ required: true, message: "请选择PLC型号" }]}
            extra="切换型号会自动带入对应的默认连接模式和参数"
          >
            <Select
              options={plcModelOptions.map(({ value, label }) => ({ value, label }))}
              onChange={(value) => handlePlcModelChange(value as S7.PlcModel)}
            />
          </Form.Item>
          <Form.Item
            name="connectionMode"
            label="连接模式"
            rules={[{ required: true, message: "请选择连接模式" }]}
            extra={connectionModeTip}
          >
            <Select
              options={connectionModeOptions}
              onChange={(value) => handleConnectionModeChange(value as S7.ConnectionMode)}
            />
          </Form.Item>
          {selectedConnectionMode === "TSAP" ? (
            <Row gutter={12}>
              <Col xs={24} sm={12}>
                <Form.Item
                  name="localTSAP"
                  label="本地 TSAP"
                  rules={[
                    { required: true, message: "请输入本地 TSAP" },
                    { validator: validateTsapValue },
                  ]}
                  extra="支持 4D57、0200、0x4D57、4D.57"
                >
                  <Input placeholder="例如 4D57" />
                </Form.Item>
              </Col>
              <Col xs={24} sm={12}>
                <Form.Item
                  name="remoteTSAP"
                  label="远端 TSAP"
                  rules={[
                    { required: true, message: "请输入远端 TSAP" },
                    { validator: validateTsapValue },
                  ]}
                  extra="S7-200 常见值为 4D57 或 0200"
                >
                  <Input placeholder="例如 4D57" />
                </Form.Item>
              </Col>
            </Row>
          ) : (
            <>
              <Form.Item
                name="connectionType"
                label="连接类型"
                rules={[{ required: true, message: "请选择连接类型" }]}
                extra={connectionTypeTip}
              >
                <Select options={connectionTypeOptions} />
              </Form.Item>
              <Row gutter={12}>
                <Col xs={24} sm={12}>
                  <Form.Item
                    name="rack"
                    label="Rack"
                    rules={[{ required: true, message: "请输入 Rack" }]}
                    extra="西门子机架号，通常从 0 开始"
                  >
                    <InputNumber min={0} className="w-full" />
                  </Form.Item>
                </Col>
                <Col xs={24} sm={12}>
                  <Form.Item
                    name="slot"
                    label="Slot"
                    rules={[{ required: true, message: "请输入 Slot" }]}
                    extra="CPU 或通信模块所在槽位"
                  >
                    <InputNumber min={0} className="w-full" />
                  </Form.Item>
                </Col>
              </Row>
            </>
          )}
          <Form.Item
            name="pollInterval"
            label="轮询间隔（秒）"
            rules={[{ required: true, message: "请输入轮询间隔" }]}
            extra="数值越小采集越频繁，建议 1~300 秒之间按实际场景调整"
          >
            <InputNumber min={1} max={3600} className="w-full" addonAfter="秒" />
          </Form.Item>
          <Form.Item name="remark" label="备注">
            <Input.TextArea rows={3} placeholder="备注说明" />
          </Form.Item>
          <Form.Item name="enabled" label="启用" valuePropName="checked">
            <Switch />
          </Form.Item>
        </Form>
      </Modal>

      <AreaModal
        open={areaModalOpen}
        mode={editingArea ? "edit" : "create"}
        initialValue={
          editingArea
            ? {
                ...editingArea,
                area:
                  normalizeAreaTypeForPlcModel(activeConfig?.plcModel, editingArea.area) ??
                  editingArea.area,
                dataType: bitOnlyAreaTypes.includes(editingArea.area as S7.AreaType)
                  ? "BOOL"
                  : editingArea.area === "CT" || editingArea.area === "TM"
                    ? "UINT16"
                    : normalizeS7DataType(editingArea.dataType),
                size: bitOnlyAreaTypes.includes(editingArea.area as S7.AreaType)
                  ? 1
                  : editingArea.area === "CT" || editingArea.area === "TM"
                    ? getDataTypeSize("UINT16")
                    : editingArea.size,
                dbNumber:
                  normalizeAreaTypeForPlcModel(activeConfig?.plcModel, editingArea.area) === "V"
                    ? undefined
                    : editingArea.dbNumber,
                startBit: editingArea.startBit,
              }
            : undefined
        }
        plcModel={activeConfig?.plcModel}
        onCancel={() => setAreaModalOpen(false)}
        onSubmit={async (value) => {
          if (!activeTypeId || !activeConfig) return;
          const nextAreas = activeConfig.areas.some((area) => area.id === value.id)
            ? activeConfig.areas.map((area) => (area.id === value.id ? value : area))
            : [...activeConfig.areas, value];
          await saveMutation.mutateAsync({
            id: activeTypeId,
            protocol: "S7",
            config: {
              ...activeConfig,
              areas: nextAreas,
            },
          });
          await refetch();
          setAreaModalOpen(false);
          setEditingAreaId(null);
        }}
      />
    </PageContainer>
  );
};

export default S7ConfigPage;
