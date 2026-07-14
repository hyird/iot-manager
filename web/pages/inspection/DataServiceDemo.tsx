import {
  ApiOutlined,
  CheckCircleFilled,
  PlayCircleOutlined,
  PlusOutlined,
  SaveOutlined,
} from "@ant-design/icons";
import {
  App,
  Button,
  Card,
  Col,
  Form,
  Input,
  Modal,
  Popconfirm,
  Row,
  Select,
  Space,
  Statistic,
  Switch,
  Table,
  Tabs,
  Tag,
  Typography,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useState } from "react";

const { Text } = Typography;

interface ServiceConfig {
  title: string;
  description: string;
  endpoint: string;
  protocol: string;
  fields: Array<{ name: string; label: string; value: string }>;
}

const configs: Record<string, ServiceConfig> = {
  storage: {
    title: "数据源入库",
    description: "将 OPC、Modbus、MQTT 实时数据统一写入数据库",
    endpoint: "PostgreSQL / iot_history",
    protocol: "OPC / Modbus / MQTT",
    fields: [
      { name: "database", label: "目标数据库", value: "iot_history" },
      { name: "table", label: "数据表", value: "device_tag_history" },
      { name: "batch", label: "批量写入条数", value: "500" },
    ],
  },
  "modbus-server": {
    title: "Modbus Server",
    description: "以 Modbus TCP Server 方式向现有控制系统发布标签数据",
    endpoint: "0.0.0.0:1502",
    protocol: "Modbus TCP",
    fields: [
      { name: "listen", label: "监听地址", value: "0.0.0.0" },
      { name: "port", label: "监听端口", value: "1502" },
      { name: "slave", label: "默认站号", value: "1" },
    ],
  },
  "opc-server": {
    title: "OPC Server-UA",
    description: "通过 OPC UA 地址空间统一发布采集标签和计算标签",
    endpoint: "opc.tcp://0.0.0.0:4840",
    protocol: "OPC UA",
    fields: [
      { name: "endpoint", label: "Endpoint", value: "opc.tcp://0.0.0.0:4840" },
      { name: "namespace", label: "Namespace", value: "urn:iot-manager:tags" },
      { name: "security", label: "安全策略", value: "Basic256Sha256" },
    ],
  },
  mqtt: {
    title: "MQTT 发布 / 订阅",
    description: "按主题发布实时数据并订阅控制与业务消息",
    endpoint: "mqtt://192.168.10.8:1883",
    protocol: "MQTT 3.1.1 / 5.0",
    fields: [
      { name: "broker", label: "Broker", value: "mqtt://192.168.10.8:1883" },
      { name: "publish", label: "发布主题", value: "factory/{device}/telemetry" },
      { name: "subscribe", label: "订阅主题", value: "factory/+/command" },
    ],
  },
  http: {
    title: "HTTP GET / POST",
    description: "通过标准 HTTP 接口查询实时数据或主动推送业务数据",
    endpoint: "/open-api/v1/realtime",
    protocol: "HTTP / JSON",
    fields: [
      { name: "path", label: "接口路径", value: "/open-api/v1/realtime" },
      { name: "method", label: "请求方式", value: "GET / POST" },
      { name: "auth", label: "认证方式", value: "AccessKey" },
    ],
  },
};

interface MappingItem {
  key: string;
  source: string;
  target: string;
  value: string;
  status: string;
}

const createMappings = (protocol: string): MappingItem[] => [
  {
    key: "1",
    source: "一厂.出水流量",
    target: `${protocol}.FLOW_OUT`,
    value: "426.8",
    status: "正常",
  },
  {
    key: "2",
    source: "一厂.清水池液位",
    target: `${protocol}.TANK_LEVEL`,
    value: "4.32",
    status: "正常",
  },
  {
    key: "3",
    source: "一厂.高压泵运行",
    target: `${protocol}.PUMP_RUN`,
    value: "true",
    status: "正常",
  },
];

export default function DataServiceDemo() {
  const { message } = App.useApp();
  const [activeKey, setActiveKey] = useState("storage");
  const config = configs[activeKey] || configs.storage;
  const [enabled, setEnabled] = useState(true);
  const [mappingGroups, setMappingGroups] = useState<Record<string, MappingItem[]>>(() =>
    Object.fromEntries(
      Object.entries(configs).map(([key, item]) => [key, createMappings(item.protocol)])
    )
  );
  const [keyword, setKeyword] = useState("");
  const [editingMapping, setEditingMapping] = useState<MappingItem | null>(null);
  const [mappingModalOpen, setMappingModalOpen] = useState(false);
  const [mappingForm] = Form.useForm();
  const data = mappingGroups[activeKey] || [];
  const filteredData = data.filter((item) =>
    `${item.source} ${item.target} ${item.value}`.toLowerCase().includes(keyword.toLowerCase())
  );

  const openMappingModal = (item?: MappingItem) => {
    setEditingMapping(item || null);
    mappingForm.setFieldsValue(item || { source: "", target: "", value: "", status: "正常" });
    setMappingModalOpen(true);
  };

  const saveMapping = async () => {
    const values = await mappingForm.validateFields();
    setMappingGroups((groups) => {
      const current = groups[activeKey] || [];
      const next = editingMapping
        ? current.map((item) => (item.key === editingMapping.key ? { ...item, ...values } : item))
        : [...current, { key: `mapping-${Date.now()}`, ...values }];
      return { ...groups, [activeKey]: next };
    });
    message.success(editingMapping ? "映射已更新（Mock）" : "映射已新增（Mock）");
    setMappingModalOpen(false);
  };
  const columns: ColumnsType<MappingItem> = [
    { title: "源标签", dataIndex: "source", render: (value) => <Text strong>{value}</Text> },
    { title: "服务映射", dataIndex: "target", render: (value) => <Text code>{value}</Text> },
    { title: "当前值", dataIndex: "value", width: 120 },
    {
      title: "状态",
      dataIndex: "status",
      width: 100,
      render: (value) => <Tag color={value === "正常" ? "success" : "default"}>{value}</Tag>,
    },
    {
      title: "操作",
      width: 130,
      render: (_, record) => (
        <Space>
          <Button type="link" size="small" onClick={() => openMappingModal(record)}>
            编辑
          </Button>
          <Popconfirm
            title="删除这条映射？"
            onConfirm={() => {
              setMappingGroups((groups) => ({
                ...groups,
                [activeKey]: (groups[activeKey] || []).filter((item) => item.key !== record.key),
              }));
              message.success("映射已删除（Mock）");
            }}
          >
            <Button type="link" danger size="small">
              删除
            </Button>
          </Popconfirm>
        </Space>
      ),
    },
  ];

  return (
    <Card
      className="mb-4 border-blue-100"
      title={
        <Space>
          <ApiOutlined className="text-blue-600" />
          数据服务接口
          <Tag color="blue">前端演示</Tag>
        </Space>
      }
      extra={
        <Space>
          <Tag
            color={enabled ? "success" : "default"}
            icon={enabled ? <CheckCircleFilled /> : undefined}
          >
            {enabled ? "服务运行中" : "服务已停止"}
          </Tag>
          <Switch checked={enabled} onChange={setEnabled} />
        </Space>
      }
    >
      <Tabs
        activeKey={activeKey}
        onChange={(key) => {
          setActiveKey(key);
          setKeyword("");
        }}
        items={Object.entries(configs).map(([key, item]) => ({
          key,
          label: item.title,
        }))}
      />

      <div className="rounded-lg bg-slate-50 px-4 py-3 mb-4 flex flex-wrap justify-between gap-2">
        <Text type="secondary">{config.description}</Text>
        <Tag color="blue">{config.endpoint}</Tag>
      </div>

      <Row gutter={[16, 16]}>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="服务状态" value={enabled ? "正常" : "停止"} />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="发布标签" value={data.length} />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="今日请求" value={18642} />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="成功率" value={99.98} suffix="%" />
          </Card>
        </Col>
      </Row>

      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} xl={9}>
          <Card title={`${config.title}配置`} className="h-full" size="small">
            <Form
              key={activeKey}
              layout="vertical"
              initialValues={Object.fromEntries(
                config.fields.map((field) => [field.name, field.value])
              )}
            >
              {config.fields.map((field) => (
                <Form.Item key={field.name} name={field.name} label={field.label}>
                  {field.name === "method" ? (
                    <Select
                      options={[
                        { value: "GET / POST", label: "GET / POST" },
                        { value: "GET", label: "GET" },
                        { value: "POST", label: "POST" },
                      ]}
                    />
                  ) : (
                    <Input />
                  )}
                </Form.Item>
              ))}
              <Space>
                <Button
                  icon={<PlayCircleOutlined />}
                  onClick={() => message.success("服务测试成功（前端演示）")}
                >
                  测试服务
                </Button>
                <Button
                  type="primary"
                  icon={<SaveOutlined />}
                  onClick={() => message.success("配置已保存（前端演示）")}
                >
                  保存配置
                </Button>
              </Space>
            </Form>
          </Card>
        </Col>
        <Col xs={24} xl={15}>
          <Card
            title="标签映射"
            size="small"
            extra={
              <Space>
                <Input.Search
                  allowClear
                  value={keyword}
                  placeholder="查询映射"
                  className="w-44"
                  onChange={(event) => setKeyword(event.target.value)}
                  onSearch={setKeyword}
                />
                <Button type="primary" icon={<PlusOutlined />} onClick={() => openMappingModal()}>
                  新增映射
                </Button>
              </Space>
            }
          >
            <Table
              columns={columns}
              dataSource={filteredData}
              pagination={{ pageSize: 5 }}
              scroll={{ x: 760 }}
            />
          </Card>
        </Col>
      </Row>

      <Modal
        title={editingMapping ? "编辑标签映射" : "新增标签映射"}
        open={mappingModalOpen}
        onOk={saveMapping}
        onCancel={() => setMappingModalOpen(false)}
        okText="保存"
        cancelText="取消"
      >
        <Form form={mappingForm} layout="vertical" className="mt-4">
          <Form.Item name="source" label="源标签" rules={[{ required: true }]}>
            <Input placeholder="一厂.出水流量" />
          </Form.Item>
          <Form.Item name="target" label="服务映射" rules={[{ required: true }]}>
            <Input placeholder={`${config.protocol}.FLOW_OUT`} />
          </Form.Item>
          <Form.Item name="value" label="模拟值" rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="status" label="状态" rules={[{ required: true }]}>
            <Select
              options={[
                { value: "正常", label: "正常" },
                { value: "停用", label: "停用" },
              ]}
            />
          </Form.Item>
        </Form>
      </Modal>
    </Card>
  );
}
