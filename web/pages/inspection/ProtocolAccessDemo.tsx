import { ApiOutlined, CheckCircleFilled, PlusOutlined, ReloadOutlined } from "@ant-design/icons";
import {
  App,
  Button,
  Card,
  Col,
  Descriptions,
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
  Tag,
  Typography,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useState } from "react";
import { useLocation } from "react-router-dom";
import { PageContainer } from "@/components/PageContainer";

const { Paragraph, Text, Title } = Typography;

interface OpcTag {
  key: string;
  name: string;
  address: string;
  type: string;
  value: string;
  quality: string;
}

const initialTags: OpcTag[] = [
  {
    key: "1",
    name: "出水瞬时流量",
    address: "Channel1.Device1.Flow",
    type: "Float",
    value: "426.8",
    quality: "Good",
  },
  {
    key: "2",
    name: "清水池液位",
    address: "Channel1.Device1.Level",
    type: "Float",
    value: "4.32",
    quality: "Good",
  },
  {
    key: "3",
    name: "高压泵运行",
    address: "Channel1.Device1.PumpRun",
    type: "Boolean",
    value: "true",
    quality: "Good",
  },
];

export default function ProtocolAccessDemo() {
  const { message } = App.useApp();
  const location = useLocation();
  const isDa = location.pathname.endsWith("opc-da");
  const title = isDa ? "OPC Client-DA" : "OPC Client-UA";
  const [enabled, setEnabled] = useState(true);
  const [form] = Form.useForm();
  const [tagForm] = Form.useForm();
  const [tagItems, setTagItems] = useState(initialTags);
  const [keyword, setKeyword] = useState("");
  const [editingTag, setEditingTag] = useState<OpcTag | null>(null);
  const [tagModalOpen, setTagModalOpen] = useState(false);

  const filteredTags = tagItems.filter((item) =>
    `${item.name} ${item.address} ${item.type}`.toLowerCase().includes(keyword.toLowerCase())
  );

  const openTagModal = (item?: OpcTag) => {
    setEditingTag(item || null);
    tagForm.setFieldsValue(
      item || { name: "", address: "", type: "Float", value: "0", quality: "Good" }
    );
    setTagModalOpen(true);
  };

  const saveTag = async () => {
    const values = await tagForm.validateFields();
    if (editingTag) {
      setTagItems((items) =>
        items.map((item) => (item.key === editingTag.key ? { ...item, ...values } : item))
      );
      message.success("标签已更新（Mock）");
    } else {
      setTagItems((items) => [...items, { key: `tag-${Date.now()}`, ...values }]);
      message.success("标签已新增（Mock）");
    }
    setTagModalOpen(false);
  };

  const columns: ColumnsType<OpcTag> = [
    { title: "标签名称", dataIndex: "name", render: (value) => <Text strong>{value}</Text> },
    { title: "节点地址", dataIndex: "address", render: (value) => <Text code>{value}</Text> },
    { title: "数据类型", dataIndex: "type", width: 120 },
    { title: "实时值", dataIndex: "value", width: 120 },
    {
      title: "质量码",
      dataIndex: "quality",
      width: 100,
      render: (value) => <Tag color={value === "Good" ? "success" : "warning"}>{value}</Tag>,
    },
    {
      title: "操作",
      width: 130,
      render: (_, record) => (
        <Space>
          <Button type="link" size="small" onClick={() => openTagModal(record)}>
            编辑
          </Button>
          <Popconfirm
            title="删除这个标签？"
            onConfirm={() => {
              setTagItems((items) => items.filter((item) => item.key !== record.key));
              message.success("标签已删除（Mock）");
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
    <PageContainer>
      <div className="flex flex-wrap items-start justify-between gap-4 mb-4">
        <div>
          <Space>
            <ApiOutlined className="text-2xl text-blue-600" />
            <Title level={3} className="!mb-0">
              {title}
            </Title>
          </Space>
          <Paragraph type="secondary" className="!mt-2 !mb-0">
            {isDa
              ? "连接传统 OPC DA 服务器并采集实时标签数据"
              : "通过 OPC UA 安全通道采集设备与仪表数据"}
          </Paragraph>
        </div>
        <Space>
          <Tag
            color={enabled ? "success" : "default"}
            icon={enabled ? <CheckCircleFilled /> : undefined}
          >
            {enabled ? "采集运行中" : "已停止"}
          </Tag>
          <Switch checked={enabled} onChange={setEnabled} />
        </Space>
      </div>

      <Row gutter={[16, 16]}>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="连接状态" value={enabled ? "正常" : "停止"} />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="采集标签" value={tagItems.length} />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="采集周期" value={1} suffix="秒" />
          </Card>
        </Col>
        <Col xs={12} lg={6}>
          <Card>
            <Statistic title="数据质量" value={100} suffix="%" />
          </Card>
        </Col>
      </Row>

      <Row gutter={[16, 16]} className="mt-4">
        <Col xs={24} xl={9}>
          <Card title="连接配置" className="h-full">
            <Form
              form={form}
              layout="vertical"
              initialValues={
                isDa
                  ? {
                      server: "Kepware.KEPServerEX.V6",
                      host: "192.168.10.35",
                      cycle: "1000",
                    }
                  : {
                      server: "opc.tcp://192.168.10.35:4840",
                      security: "SignAndEncrypt",
                      cycle: "1000",
                    }
              }
            >
              <Form.Item name="server" label={isDa ? "ProgID" : "服务器地址"}>
                <Input />
              </Form.Item>
              {isDa ? (
                <Form.Item name="host" label="服务器主机">
                  <Input />
                </Form.Item>
              ) : (
                <Form.Item name="security" label="安全模式">
                  <Select
                    options={[
                      { value: "None", label: "None" },
                      { value: "Sign", label: "Sign" },
                      { value: "SignAndEncrypt", label: "SignAndEncrypt" },
                    ]}
                  />
                </Form.Item>
              )}
              <Form.Item name="cycle" label="采集周期（毫秒）">
                <Input type="number" />
              </Form.Item>
              <Space>
                <Button
                  icon={<ReloadOutlined />}
                  onClick={() => message.success("连接测试成功（前端演示）")}
                >
                  测试连接
                </Button>
                <Button type="primary" onClick={() => message.success("配置已保存（前端演示）")}>
                  保存配置
                </Button>
              </Space>
            </Form>
          </Card>
        </Col>
        <Col xs={24} xl={15}>
          <Card
            title="采集标签"
            extra={
              <Space>
                <Input.Search
                  allowClear
                  placeholder="查询标签"
                  className="w-48"
                  onSearch={setKeyword}
                  onChange={(event) => setKeyword(event.target.value)}
                />
                <Button type="primary" icon={<PlusOutlined />} onClick={() => openTagModal()}>
                  添加标签
                </Button>
              </Space>
            }
          >
            <Table
              columns={columns}
              dataSource={filteredTags}
              pagination={{ pageSize: 5 }}
              scroll={{ x: 760 }}
            />
          </Card>
        </Col>
      </Row>

      <Card size="small" className="mt-4 !bg-slate-50">
        <Descriptions
          size="small"
          column={{ xs: 1, md: 3 }}
          items={[
            { key: "1", label: "最近通讯", children: "刚刚" },
            { key: "2", label: "成功采集", children: "18,642 次" },
            { key: "3", label: "异常次数", children: "0 次" },
          ]}
        />
      </Card>

      <Modal
        title={editingTag ? "编辑采集标签" : "新增采集标签"}
        open={tagModalOpen}
        onOk={saveTag}
        onCancel={() => setTagModalOpen(false)}
        okText="保存"
        cancelText="取消"
      >
        <Form form={tagForm} layout="vertical" className="mt-4">
          <Form.Item name="name" label="标签名称" rules={[{ required: true }]}>
            <Input placeholder="例如：出水瞬时流量" />
          </Form.Item>
          <Form.Item name="address" label="节点地址" rules={[{ required: true }]}>
            <Input placeholder="Channel1.Device1.Flow" />
          </Form.Item>
          <Form.Item name="type" label="数据类型" rules={[{ required: true }]}>
            <Select
              options={["Float", "Integer", "Boolean", "String"].map((value) => ({
                value,
                label: value,
              }))}
            />
          </Form.Item>
          <Form.Item name="value" label="模拟值" rules={[{ required: true }]}>
            <Input />
          </Form.Item>
          <Form.Item name="quality" hidden>
            <Input />
          </Form.Item>
        </Form>
      </Modal>
    </PageContainer>
  );
}
