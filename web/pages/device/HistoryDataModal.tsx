/**
 * 历史数据弹窗组件
 */

import { Button, DatePicker, Form, Image, Modal, Space, Table, Tag } from "antd";
import type { ColumnsType } from "antd/es/table";
import { useCallback, useState } from "react";
import { deviceApi } from "@/services";
import type { Device, HistoryDataType } from "@/types";
import { getDefaultTimeRange, makeRecordKey, resolveElementDisplay } from "./utils";

const { RangePicker } = DatePicker;

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

const HistoryDataModal = ({ open, device, onClose }: HistoryDataModalProps) => {
  const [historyForm] = Form.useForm();
  const [funcMap, setFuncMap] = useState<Record<string, FuncState>>({});
  const [expandedFuncKeys, setExpandedFuncKeys] = useState<React.Key[]>([]);
  const [recordMap, setRecordMap] = useState<Record<string, RecordState>>({});

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
          code,
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

  // 打开弹窗时初始化
  const handleAfterOpen = useCallback(
    (isOpen: boolean) => {
      if (isOpen && device) {
        setFuncMap({});
        setRecordMap({});
        setExpandedFuncKeys([]);
        historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
        fetchFuncList(device.code, undefined, undefined, true);
      }
    },
    [device, historyForm, fetchFuncList]
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
    if (!device) return null;
    const state = funcMap[device.code];
    const funcs = state?.list ?? [];

    const dataSource: HistoryFuncRow[] = funcs.map((f) => ({
      ...f,
      deviceCode: device.code,
      key: `${device.code}_${f.funcCode}_${f.dataType}`,
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
          onChange: (p, ps) => fetchFuncList(device.code, p, ps),
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

  return (
    <Modal
      open={open}
      title={`历史数据 - ${device?.deviceName || ""}`}
      onCancel={onClose}
      footer={null}
      width="80%"
      destroyOnHidden
      afterOpenChange={handleAfterOpen}
      styles={{
        body: { height: "70vh", display: "flex", flexDirection: "column", overflow: "hidden" },
      }}
    >
      <Form
        form={historyForm}
        layout="inline"
        className="mb-4 shrink-0"
        onFinish={() => device && fetchFuncList(device.code, 1)}
      >
        <Form.Item
          label="时间范围"
          name="timeRange"
          rules={[{ required: true, message: "请选择时间范围" }]}
        >
          <RangePicker showTime className="!w-[340px]" />
        </Form.Item>
        <Form.Item>
          <Space>
            <Button type="primary" htmlType="submit">
              查询
            </Button>
            <Button
              onClick={() => {
                historyForm.setFieldsValue({ timeRange: getDefaultTimeRange() });
                if (device) fetchFuncList(device.code, 1);
              }}
            >
              重置
            </Button>
          </Space>
        </Form.Item>
      </Form>
      <div className="flex-1 overflow-auto">{renderHistoryFuncTable()}</div>
    </Modal>
  );
};

export default HistoryDataModal;
