/**
 * 历史数据弹窗组件
 */

import { Button, Checkbox, DatePicker, Form, Image, Modal, Space, Table, Tabs, Tag } from "antd";
import type { ColumnsType } from "antd/es/table";
import dayjs from "dayjs";
import { LineChart } from "echarts/charts";
import { DataZoomComponent, GridComponent, TooltipComponent } from "echarts/components";
import * as echarts from "echarts/core";
import { SVGRenderer } from "echarts/renderers";
import ReactEChartsCore from "echarts-for-react/lib/core";
import { useCallback, useMemo, useState } from "react";

echarts.use([LineChart, GridComponent, TooltipComponent, DataZoomComponent, SVGRenderer]);

import { deviceApi } from "@/services";
import type { Device, HistoryDataType } from "@/types";
import { getDefaultTimeRange, makeRecordKey, resolveElementDisplay } from "./utils";

const { RangePicker } = DatePicker;

const timePresets = [
  {
    label: "最近1小时",
    value: [dayjs().subtract(1, "hour"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs],
  },
  {
    label: "最近6小时",
    value: [dayjs().subtract(6, "hour"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs],
  },
  { label: "今天", value: [dayjs().startOf("day"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs] },
  {
    label: "最近3天",
    value: [dayjs().subtract(3, "day").startOf("day"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs],
  },
  {
    label: "最近7天",
    value: [dayjs().subtract(7, "day").startOf("day"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs],
  },
  { label: "本月", value: [dayjs().startOf("month"), dayjs()] as [dayjs.Dayjs, dayjs.Dayjs] },
];

// ========== 类型 ==========

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

interface ElementRowType {
  key: string;
  reportTime: string;
  elements: Device.Element[];
  direction?: "UP" | "DOWN";
  responseId?: number;
  responseElements?: Device.Element[];
  userId?: number;
  userName?: string;
}

interface ImageRowType {
  key: string;
  reportTime: string;
  size: number;
  data: string;
}

// ========== Props ==========

interface HistoryDataModalProps {
  open: boolean;
  device: Device.RealTimeData | null;
  onClose: () => void;
}

// ========== 渲染辅助 ==========

const renderElementValue = (el: Device.Element | undefined) => {
  const result = resolveElementDisplay(el);
  if (result.type === "bits") {
    return (
      <Space size={4} wrap>
        {result.labels.map((label, i) => (
          <Tag key={i} color="blue">
            {label}
          </Tag>
        ))}
      </Space>
    );
  }
  return result.value;
};

// ========== 组件 ==========

const MODBUS_FUNC_CODE = "MODBUS_READ";

const HistoryDataModal = ({ open, device, onClose }: HistoryDataModalProps) => {
  const [historyForm] = Form.useForm();
  const [funcMap, setFuncMap] = useState<Record<string, FuncState>>({});
  const [expandedFuncKeys, setExpandedFuncKeys] = useState<React.Key[]>([]);
  const [recordMap, setRecordMap] = useState<Record<string, RecordState>>({});
  const [activeTab, setActiveTab] = useState<"table" | "chart">("table");
  const [selectedElements, setSelectedElements] = useState<string[]>([]);

  const isModbus = device?.protocol_type === "Modbus";

  const getTimeRangeParams = useCallback(() => {
    const values = historyForm.getFieldsValue();
    let startTime: Date;
    let endTime: Date;
    if (values.timeRange?.length === 2) {
      startTime = new Date(values.timeRange[0].toISOString());
      endTime = new Date(values.timeRange[1].toISOString());
    } else {
      const defaultRange = getDefaultTimeRange();
      startTime = new Date(defaultRange[0].toISOString());
      endTime = new Date(defaultRange[1].toISOString());
    }
    return { startTime, endTime };
  }, [historyForm]);

  const fetchFuncRecords = useCallback(
    async (opts: {
      code: string;
      deviceId?: number;
      funcCode: string;
      dataType: HistoryDataType;
      pageArg?: number;
      pageSizeArg?: number;
    }) => {
      const { code, funcCode, dataType } = opts;
      const key = makeRecordKey(code, funcCode, dataType);

      setRecordMap((prev) => ({
        ...prev,
        [key]: prev[key]
          ? { ...prev[key], loading: true }
          : {
              loading: true,
              dataType,
              list: [],
              page: opts.pageArg ?? 1,
              pageSize: opts.pageSizeArg ?? 10,
              total: 0,
            },
      }));

      try {
        const { startTime, endTime } = getTimeRangeParams();
        const res = await deviceApi.getHistoryData({
          code: opts.deviceId ? undefined : code,
          deviceId: opts.deviceId,
          funcCode,
          dataType,
          page: opts.pageArg ?? 1,
          pageSize: opts.pageSizeArg ?? 10,
          startTime,
          endTime,
        });
        setRecordMap((prev) => ({
          ...prev,
          [key]: {
            loading: false,
            dataType,
            list:
              dataType === "ELEMENT"
                ? (res.list as Device.HistoryElement[])
                : (res.list as Device.HistoryImage[]),
            page: opts.pageArg ?? 1,
            pageSize: opts.pageSizeArg ?? 10,
            total: res.total || res.list?.length || 0,
          },
        }));
      } catch {
        setRecordMap((prev) => ({
          ...prev,
          [key]: prev[key]
            ? { ...prev[key], loading: false }
            : {
                loading: false,
                dataType,
                list: [],
                page: opts.pageArg ?? 1,
                pageSize: opts.pageSizeArg ?? 10,
                total: 0,
              },
        }));
      }
    },
    [getTimeRangeParams]
  );

  const fetchFuncList = useCallback(
    async (code: string, pageArg?: number, pageSizeArg?: number, autoExpand = false) => {
      const current = pageArg ?? 1;
      const size = pageSizeArg ?? 10;

      setFuncMap((prev) => ({
        ...prev,
        [code]: {
          ...(prev[code] ?? { list: [], page: current, pageSize: size, total: 0 }),
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
        setFuncMap((prev) => ({
          ...prev,
          [code]: {
            loading: false,
            list,
            page: current,
            pageSize: size,
            total: res.total || list.length || 0,
          },
        }));

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
        setFuncMap((prev) => ({
          ...prev,
          [code]: {
            ...(prev[code] ?? { list: [], page: current, pageSize: size, total: 0 }),
            loading: false,
          },
        }));
      }
    },
    [getTimeRangeParams, fetchFuncRecords]
  );

  /** Modbus 直接查询历史记录（跳过功能码层级，用 deviceId 查询） */
  const fetchModbusRecords = useCallback(
    (deviceId: number, pageArg?: number, pageSizeArg?: number) => {
      fetchFuncRecords({
        code: String(deviceId),
        deviceId,
        funcCode: MODBUS_FUNC_CODE,
        dataType: "ELEMENT",
        pageArg,
        pageSizeArg,
      });
    },
    [fetchFuncRecords]
  );

  // 打开弹窗时初始化
  const handleAfterOpen = useCallback(
    (isOpen: boolean) => {
      if (isOpen && device) {
        setFuncMap({});
        setRecordMap({});
        setExpandedFuncKeys([]);
        historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
        if (isModbus) {
          fetchModbusRecords(device.id);
        } else {
          fetchFuncList(device.device_code ?? "", undefined, undefined, true);
        }
      }
    },
    [device, isModbus, historyForm, fetchFuncList, fetchModbusRecords]
  );

  // ========== 表格渲染 ==========

  const renderElementTable = (func: HistoryFuncRow) => {
    const key = makeRecordKey(func.deviceCode, func.funcCode, func.dataType);
    const rs = recordMap[key];
    const records = (rs?.list as Device.HistoryElement[]) || [];
    const elementNameSet = new Set<string>();
    for (const r of records) {
      for (const e of r.elements) {
        elementNameSet.add(e.name);
      }
    }
    const elementNames = Array.from(elementNameSet);

    const hasDownData = records.some((r) => r.direction === "DOWN");

    const responseElementNames = new Set<string>();
    for (const r of records) {
      if (r.responseElements) {
        for (const e of r.responseElements) {
          responseElementNames.add(e.name);
        }
      }
    }

    const dataSource: ElementRowType[] = records.map((r, idx) => ({
      key: `${idx}_${new Date(r.reportTime).getTime()}`,
      reportTime: new Date(r.reportTime).toLocaleString(),
      elements: r.elements,
      direction: r.direction,
      responseId: r.responseId,
      responseElements: r.responseElements,
      userId: r.userId,
      userName: r.userName,
    }));

    const cols: ColumnsType<ElementRowType> = [
      { title: "时间", dataIndex: "reportTime", key: "reportTime", fixed: "left", width: 180 },
      {
        title: "方向",
        dataIndex: "direction",
        key: "direction",
        width: 80,
        render: (dir?: "UP" | "DOWN") =>
          dir === "DOWN" ? <Tag color="orange">下行</Tag> : <Tag color="green">上行</Tag>,
      },
      ...(hasDownData
        ? [
            {
              title: "发送人",
              dataIndex: "userName",
              key: "userName",
              width: 100,
              render: (_: unknown, record: ElementRowType) =>
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
        render: (els: ElementRowType["elements"]) => {
          const el = els.find((e) => e.name === name);
          return renderElementValue(el);
        },
      })),
      ...(hasDownData
        ? [
            {
              title: "应答状态",
              key: "responseStatus",
              width: 100,
              render: (_: unknown, record: ElementRowType) => {
                if (record.direction !== "DOWN") return "-";
                return record.responseId ? (
                  <Tag color="success">已应答</Tag>
                ) : (
                  <Tag color="warning">待应答</Tag>
                );
              },
            },
            ...Array.from(responseElementNames).map((name) => ({
              title: `应答-${name}`,
              key: `response_${name}`,
              width: 150,
              ellipsis: true,
              render: (_: unknown, record: ElementRowType) => {
                if (record.direction !== "DOWN" || !record.responseElements) return "-";
                const el = record.responseElements.find((e) => e.name === name);
                return renderElementValue(el);
              },
            })),
          ]
        : []),
    ];

    const baseWidth = 180 + 80;
    const elementsWidth = elementNames.length * 150;
    const downWidth = hasDownData ? 100 + 100 + Array.from(responseElementNames).length * 150 : 0;
    const tableWidth = Math.max(baseWidth + elementsWidth + downWidth, 800);

    return (
      <Table<ElementRowType>
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
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 条`,
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

    const dataSource: ImageRowType[] = records.map((r, idx) => ({
      key: `${idx}_${new Date(r.reportTime).getTime()}`,
      reportTime: new Date(r.reportTime).toLocaleString(),
      size: r.size,
      data: r.data,
    }));

    const cols: ColumnsType<ImageRowType> = [
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
      <Table<ImageRowType>
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
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 条`,
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

  /** Modbus 扁平历史表格（无功能码层级） */
  const renderModbusHistoryTable = () => {
    if (!device) return null;
    const key = makeRecordKey(String(device.id), MODBUS_FUNC_CODE, "ELEMENT");
    const rs = recordMap[key];
    const records = (rs?.list as Device.HistoryElement[]) || [];

    const elementNameSet = new Set<string>();
    for (const r of records) {
      for (const e of r.elements) {
        elementNameSet.add(e.name);
      }
    }
    const elementNames = Array.from(elementNameSet);

    const dataSource: ElementRowType[] = records.map((r, idx) => ({
      key: `${idx}_${new Date(r.reportTime).getTime()}`,
      reportTime: new Date(r.reportTime).toLocaleString(),
      elements: r.elements,
    }));

    const cols: ColumnsType<ElementRowType> = [
      { title: "时间", dataIndex: "reportTime", key: "reportTime", fixed: "left", width: 180 },
      ...elementNames.map((name) => ({
        title: name,
        dataIndex: "elements",
        key: name,
        width: 150,
        ellipsis: true,
        render: (els: ElementRowType["elements"]) => {
          const el = els.find((e) => e.name === name);
          return renderElementValue(el);
        },
      })),
    ];

    const tableWidth = Math.max(180 + elementNames.length * 150, 600);

    return (
      <Table<ElementRowType>
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
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 条`,
          onChange: (p, ps) => fetchModbusRecords(device.id, p, ps),
        }}
      />
    );
  };

  const renderHistoryFuncTable = () => {
    if (!device) return null;
    const state = funcMap[device.device_code ?? ""];
    const funcs = state?.list ?? [];

    const dataSource: HistoryFuncRow[] = funcs.map((f) => ({
      ...f,
      deviceCode: device.device_code ?? "",
      key: `${device.device_code ?? ""}_${f.funcCode}_${f.dataType}`,
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
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 个功能码`,
          onChange: (p, ps) => fetchFuncList(device.device_code ?? "", p, ps),
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

  // ========== 图表数据准备 ==========

  const chartData = useMemo(() => {
    if (!device) return null;
    const key = isModbus
      ? makeRecordKey(String(device.id), MODBUS_FUNC_CODE, "ELEMENT")
      : expandedFuncKeys.length > 0
        ? String(expandedFuncKeys[0])
        : null;

    if (!key) return null;

    const rs = recordMap[key];
    const records = (rs?.list as Device.HistoryElement[]) || [];

    // 提取所有测点名称
    const elementNameSet = new Set<string>();
    for (const r of records) {
      for (const e of r.elements) {
        elementNameSet.add(e.name);
      }
    }

    const elementNames = Array.from(elementNameSet);

    // 初始化：默认全选所有测点
    if (elementNames.length > 0 && selectedElements.length === 0) {
      setSelectedElements(elementNames);
    }

    return {
      records,
      elementNames,
    };
  }, [device, isModbus, recordMap, expandedFuncKeys, selectedElements.length]);

  // ========== 图表渲染 ==========

  const renderChart = () => {
    if (!chartData || chartData.records.length === 0) {
      return <div className="flex items-center justify-center h-full text-gray-400">暂无数据</div>;
    }

    const { records } = chartData;

    // 使用选中的测点（已在 useMemo 中初始化为全选）
    const displayElements = selectedElements;

    // 准备时间轴数据
    const timeData = records.map((r) => new Date(r.reportTime).toLocaleString("zh-CN"));

    // 准备每个测点的数据系列
    const series = displayElements.map((name, index) => {
      const data = records.map((r) => {
        const el = r.elements.find((e) => e.name === name);
        if (!el || !el.value) return null;
        // 尝试转换为数字
        const numValue = Number.parseFloat(String(el.value));
        return Number.isNaN(numValue) ? null : numValue;
      });

      const colors = ["#1677ff", "#52c41a", "#faad14", "#ff4d4f", "#722ed1", "#13c2c2"];

      return {
        name,
        type: "line",
        smooth: true,
        data,
        itemStyle: {
          color: colors[index % colors.length],
        },
      };
    });

    return (
      <div className="space-y-4">
        {/* 测点选择器 */}
        <div className="p-3 bg-gray-50 rounded">
          <div className="text-sm text-gray-600 mb-2">选择要显示的测点（默认全选）：</div>
          <Checkbox.Group
            options={chartData.elementNames.map((name) => ({ label: name, value: name }))}
            value={selectedElements}
            onChange={(values) => setSelectedElements(values as string[])}
          />
        </div>

        {/* 图表 */}
        <ReactEChartsCore
          echarts={echarts}
          option={{
            tooltip: {
              trigger: "axis",
              axisPointer: {
                type: "cross",
              },
            },
            grid: {
              left: "3%",
              right: "4%",
              bottom: "3%",
              top: "10%",
              containLabel: true,
            },
            xAxis: {
              type: "category",
              data: timeData,
              axisLabel: {
                rotate: 45,
                fontSize: 10,
              },
            },
            yAxis: {
              type: "value",
              axisLabel: {
                fontSize: 11,
              },
            },
            series,
            dataZoom: [
              {
                type: "inside",
                start: 0,
                end: 100,
              },
              {
                start: 0,
                end: 100,
              },
            ],
          }}
          style={{ height: "500px" }}
          opts={{ renderer: "svg" }}
        />
      </div>
    );
  };

  return (
    <Modal
      open={open}
      title={`历史数据 - ${device?.name || ""}`}
      onCancel={onClose}
      footer={null}
      width="85%"
      destroyOnHidden
      afterOpenChange={handleAfterOpen}
      styles={{
        body: { height: "75vh", display: "flex", flexDirection: "column", overflow: "hidden" },
      }}
    >
      <Form
        form={historyForm}
        layout="inline"
        className="mb-4 shrink-0"
        onFinish={() => {
          if (!device) return;
          if (isModbus) fetchModbusRecords(device.id);
          else fetchFuncList(device.device_code ?? "", 1);
        }}
      >
        <Form.Item
          label="时间范围"
          name="timeRange"
          rules={[{ required: true, message: "请选择时间范围" }]}
        >
          <RangePicker showTime presets={timePresets} className="!w-[340px]" />
        </Form.Item>
        <Form.Item>
          <Space>
            <Button type="primary" htmlType="submit">
              查询
            </Button>
            <Button
              onClick={() => {
                historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
                if (!device) return;
                if (isModbus) fetchModbusRecords(device.id);
                else fetchFuncList(device.device_code ?? "", 1);
              }}
            >
              重置
            </Button>
          </Space>
        </Form.Item>
      </Form>

      <Tabs
        activeKey={activeTab}
        onChange={(key) => setActiveTab(key as "table" | "chart")}
        className="flex-1 overflow-hidden"
        items={[
          {
            key: "table",
            label: "数据表格",
            children: (
              <div className="overflow-auto" style={{ height: "calc(75vh - 150px)" }}>
                {isModbus ? renderModbusHistoryTable() : renderHistoryFuncTable()}
              </div>
            ),
          },
          {
            key: "chart",
            label: "数据图表",
            children: (
              <div className="overflow-auto p-4" style={{ height: "calc(75vh - 150px)" }}>
                {renderChart()}
              </div>
            ),
          },
        ]}
      />
    </Modal>
  );
};

export default HistoryDataModal;
