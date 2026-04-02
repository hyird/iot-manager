/**
 * 历史数据弹窗组件（协议无关）
 *
 * 扁平化展示：不区分功能码分组，所有记录混合展示，通过"功能码"列区分来源
 * Tabs: 要素数据 | 图片数据 | 数据图表
 */

import {
  Button,
  Checkbox,
  DatePicker,
  Form,
  Image,
  Modal,
  Space,
  Spin,
  Table,
  Tabs,
  Tag,
} from "antd";
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
import { getDefaultTimeRange, resolveElementDisplay } from "./utils";

const { RangePicker } = DatePicker;

/** 每次调用时生成新鲜的时间预设（避免模块加载时 dayjs() 固化） */
const getTimePresets = () => [
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

interface RecordState<T> {
  loading: boolean;
  list: T[];
  page: number;
  pageSize: number;
  total: number;
}

interface ElementRowType {
  key: string;
  reportTime: string;
  elements: Device.Element[];
  direction?: "UP" | "DOWN";
  status?: string;
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

// ========== Raw JSONB 透传适配 ==========

/** 后端非分页查询返回的 Raw 格式（JSONB 原样透传） */
interface RawElementRecord {
  reportTime: string;
  data: Record<string, { name: string; value: string | number | null; unit?: string }>;
}

/** 将 Raw JSONB 格式转换为图表消费的 HistoryElement 格式 */
const convertRawToElements = (list: unknown[]): Device.HistoryElement[] =>
  (list as RawElementRecord[]).map((r) => ({
    reportTime: r.reportTime,
    elements: Object.values(r.data || {}).map((e) => ({
      name: e.name,
      value: e.value,
      unit: e.unit,
    })),
  }));

// ========== 组件 ==========

/** 基于 HSL 色轮均匀分布生成 N 个高辨识度颜色（黄金角偏移避免相邻色相近） */
const generateChartColors = (count: number): string[] => {
  const colors: string[] = [];
  const goldenAngle = 137.508;
  for (let i = 0; i < count; i++) {
    const hue = (i * goldenAngle) % 360;
    colors.push(`hsl(${Math.round(hue)}, 70%, 50%)`);
  }
  return colors;
};

const emptyElementState: RecordState<Device.HistoryElement> = {
  loading: false,
  list: [],
  page: 1,
  pageSize: 10,
  total: 0,
};

const emptyImageState: RecordState<Device.HistoryImage> = {
  loading: false,
  list: [],
  page: 1,
  pageSize: 10,
  total: 0,
};

const HistoryDataModal = ({ open, device, onClose }: HistoryDataModalProps) => {
  const [historyForm] = Form.useForm();
  const [elementState, setElementState] =
    useState<RecordState<Device.HistoryElement>>(emptyElementState);
  const [imageState, setImageState] = useState<RecordState<Device.HistoryImage>>(emptyImageState);
  const [activeTab, setActiveTab] = useState<"element" | "image" | "chart">("element");
  const [selectedElements, setSelectedElements] = useState<string[]>([]);
  const [chartRecords, setChartRecords] = useState<Device.HistoryElement[]>([]);
  const [chartLoading, setChartLoading] = useState(false);

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

  /** 查询记录（按 dataType 查所有记录） */
  const fetchRecords = useCallback(
    async (dataType: HistoryDataType, pageArg?: number, pageSizeArg?: number) => {
      if (!device) return;
      const page = pageArg ?? 1;
      const pageSize = pageSizeArg ?? 10;
      const isElement = dataType === "ELEMENT";

      if (isElement) {
        setElementState((prev) => ({ ...prev, loading: true, page, pageSize }));
      } else {
        setImageState((prev) => ({ ...prev, loading: true, page, pageSize }));
      }

      try {
        const { startTime, endTime } = getTimeRangeParams();
        const res = await deviceApi.getHistoryData({
          deviceId: device.id,
          dataType,
          page,
          pageSize,
          startTime,
          endTime,
        });
        const state = {
          loading: false,
          page,
          pageSize,
          total: res.total || res.list?.length || 0,
        };
        if (isElement) {
          setElementState({ ...state, list: res.list as Device.HistoryElement[] });
        } else {
          setImageState({ ...state, list: res.list as Device.HistoryImage[] });
        }
      } catch {
        if (isElement) {
          setElementState((prev) => ({ ...prev, loading: false }));
        } else {
          setImageState((prev) => ({ ...prev, loading: false }));
        }
      }
    },
    [device, getTimeRangeParams]
  );

  /** 获取图表数据（查所有要素记录） */
  const fetchChartRecords = useCallback(async () => {
    if (!device) return;
    setChartLoading(true);
    setSelectedElements([]);
    try {
      const { startTime, endTime } = getTimeRangeParams();
      const res = await deviceApi.getHistoryData({
        deviceId: device.id,
        dataType: "ELEMENT",
        startTime,
        endTime,
      });
      setChartRecords(convertRawToElements(res.list || []));
    } catch {
      setChartRecords([]);
    } finally {
      setChartLoading(false);
    }
  }, [device, getTimeRangeParams]);

  // 打开弹窗时初始化
  const handleAfterOpen = useCallback(
    (isOpen: boolean) => {
      if (isOpen && device) {
        setElementState(emptyElementState);
        setImageState(emptyImageState);
        setChartRecords([]);
        setSelectedElements([]);
        setActiveTab("element");
        historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
        fetchRecords("ELEMENT");
      }
    },
    [device, historyForm, fetchRecords]
  );

  // ========== 要素表格 ==========

  const renderElementTable = () => {
    if (!device) return null;
    const records = elementState.list;

    // 收集所有要素名称
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
      status: r.status,
      responseId: r.responseId,
      responseElements: r.responseElements,
      userId: r.userId,
      userName: r.userName,
    }));

    const cols: ColumnsType<ElementRowType> = [
      { title: "时间", dataIndex: "reportTime", key: "reportTime", fixed: "left", width: 180 },
      ...(hasDownData
        ? [
            {
              title: "方向",
              dataIndex: "direction",
              key: "direction",
              width: 80,
              render: (dir?: "UP" | "DOWN") =>
                dir === "DOWN" ? <Tag color="orange">下行</Tag> : <Tag color="green">上行</Tag>,
            },
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
                if (record.status === "SUCCESS") return <Tag color="success">成功</Tag>;
                if (record.status === "TIMEOUT") return <Tag color="error">超时</Tag>;
                if (record.status === "SEND_FAILED") return <Tag color="error">发送失败</Tag>;
                return <Tag color="warning">待应答</Tag>;
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

    const baseWidth = 180;
    const dirWidth = hasDownData ? 80 + 100 : 0;
    const elementsWidth = elementNames.length * 150;
    const downWidth = hasDownData ? 100 + Array.from(responseElementNames).length * 150 : 0;
    const tableWidth = Math.max(baseWidth + dirWidth + elementsWidth + downWidth, 800);

    return (
      <Table<ElementRowType>
        size="small"
        bordered
        loading={elementState.loading}
        columns={cols}
        dataSource={dataSource}
        scroll={{ x: tableWidth }}
        pagination={{
          current: elementState.page,
          pageSize: elementState.pageSize,
          total: elementState.total,
          showSizeChanger: true,
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 条`,
          onChange: (p, ps) => fetchRecords("ELEMENT", p, ps),
        }}
      />
    );
  };

  // ========== 图片表格 ==========

  const renderImageTable = () => {
    if (!device) return null;
    const records = imageState.list;

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
        loading={imageState.loading}
        columns={cols}
        dataSource={dataSource}
        scroll={{ x: 600 }}
        pagination={{
          current: imageState.page,
          pageSize: imageState.pageSize,
          total: imageState.total,
          showSizeChanger: true,
          showTotal: (t, range) => `${range[0]}-${range[1]} / 共 ${t} 条`,
          onChange: (p, ps) => fetchRecords("IMAGE", p, ps),
        }}
      />
    );
  };

  // ========== 图表数据准备 ==========

  const chartData = useMemo(() => {
    if (!device || chartRecords.length === 0) return null;

    const elementNames = Array.from(
      new Set((device.elements || []).map((el) => el.name.trim()).filter(Boolean))
    );

    // 初始化：默认全选所有测点
    if (elementNames.length > 0 && selectedElements.length === 0) {
      setSelectedElements(elementNames);
    }

    return {
      records: chartRecords,
      elementNames,
    };
  }, [chartRecords, device, selectedElements.length]);

  // ========== 图表渲染 ==========

  const renderChart = () => {
    if (chartLoading) {
      return (
        <div className="flex items-center justify-center h-full">
          <Spin />
        </div>
      );
    }
    if (!chartData || chartData.records.length === 0) {
      return <div className="flex items-center justify-center h-full text-gray-400">暂无数据</div>;
    }

    const { records } = chartData;

    const displayElements = selectedElements;
    const chartColors = generateChartColors(chartData.elementNames.length);

    const series = displayElements.map((name) => {
      const colorIndex = chartData.elementNames.indexOf(name);
      return {
        name,
        type: "line",
        smooth: true,
        showSymbol: false,
        data: records.map((r) => {
          const el = r.elements.find((e) => e.name === name);
          const numValue = el?.value != null ? Number.parseFloat(String(el.value)) : Number.NaN;
          return [new Date(r.reportTime).getTime(), Number.isNaN(numValue) ? null : numValue];
        }),
        itemStyle: { color: chartColors[colorIndex % chartColors.length] },
      };
    });

    return (
      <div className="flex flex-col h-full">
        {/* 测点选择器 */}
        <div className="p-3 bg-gray-50 rounded shrink-0">
          <Checkbox.Group
            value={selectedElements}
            onChange={(values) => setSelectedElements(values as string[])}
          >
            <Space wrap>
              {chartData.elementNames.map((name, i) => (
                <Checkbox key={name} value={name}>
                  <span className="inline-flex items-center">
                    <span
                      className="w-2.5 h-2.5 rounded-sm mr-1.5 shrink-0"
                      style={{ backgroundColor: chartColors[i % chartColors.length] }}
                    />
                    {name}
                  </span>
                </Checkbox>
              ))}
            </Space>
          </Checkbox.Group>
        </div>

        {/* 图表 */}
        <ReactEChartsCore
          echarts={echarts}
          notMerge
          option={{
            tooltip: {
              trigger: "axis",
              axisPointer: { type: "cross" },
            },
            grid: {
              left: "3%",
              right: "4%",
              bottom: 50,
              top: "8%",
              containLabel: true,
            },
            xAxis: {
              type: "time",
              axisLabel: { fontSize: 10 },
            },
            yAxis: {
              type: "value",
              axisLabel: { fontSize: 11 },
            },
            series,
            dataZoom: [
              { type: "inside", start: 0, end: 100 },
              { start: 0, end: 100 },
            ],
          }}
          style={{ height: "100%", minHeight: "300px" }}
          opts={{ renderer: "svg" }}
          className="flex-1"
        />
      </div>
    );
  };

  /** 按当前 tab 执行查询 */
  const handleQuery = useCallback(() => {
    if (!device) return;
    if (activeTab === "chart") {
      fetchChartRecords();
    } else if (activeTab === "image") {
      fetchRecords("IMAGE", 1);
    } else {
      fetchRecords("ELEMENT", 1);
    }
  }, [device, activeTab, fetchRecords, fetchChartRecords]);

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
      <Form form={historyForm} layout="inline" className="mb-4 shrink-0" onFinish={handleQuery}>
        <Form.Item
          label="时间范围"
          name="timeRange"
          rules={[{ required: true, message: "请选择时间范围" }]}
        >
          <RangePicker showTime presets={getTimePresets()} className="!w-[340px]" />
        </Form.Item>
        <Form.Item>
          <Space>
            <Button type="primary" htmlType="submit">
              查询
            </Button>
            <Button
              onClick={() => {
                historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
                handleQuery();
              }}
            >
              重置
            </Button>
          </Space>
        </Form.Item>
      </Form>

      <Tabs
        activeKey={activeTab}
        onChange={(key) => {
          const tab = key as "element" | "image" | "chart";
          setActiveTab(tab);
          if (!device) return;
          if (tab === "chart") {
            fetchChartRecords();
          } else if (tab === "image" && imageState.list.length === 0 && !imageState.loading) {
            fetchRecords("IMAGE");
          }
        }}
        className="flex-1 overflow-hidden"
        items={[
          {
            key: "element",
            label: "要素数据",
            children: (
              <div className="overflow-auto" style={{ height: "calc(75vh - 150px)" }}>
                {renderElementTable()}
              </div>
            ),
          },
          {
            key: "image",
            label: "图片数据",
            children: (
              <div className="overflow-auto" style={{ height: "calc(75vh - 150px)" }}>
                {renderImageTable()}
              </div>
            ),
          },
          {
            key: "chart",
            label: "数据图表",
            children: (
              <div className="flex flex-col" style={{ height: "calc(75vh - 150px)" }}>
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
