import { CheckOutlined, LinkOutlined, PlusOutlined } from "@ant-design/icons";
import {
  App,
  Button,
  Card,
  Form,
  Input,
  Modal,
  Popconfirm,
  Space,
  Switch,
  Table,
  Tag,
  Typography,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useState } from "react";
import { PageContainer } from "@/components/PageContainer";

const { Text, Title } = Typography;

interface TagLink {
  key: string;
  target: string;
  expression: string;
  source: string;
  value: string;
  enabled: boolean;
}

const initialItems: TagLink[] = [
  {
    key: "1",
    target: "一厂.出水瞬时流量",
    expression: "TAG('PLC1.FLOW_OUT') * 0.1",
    source: "PLC1.FLOW_OUT",
    value: "426.8 m³/h",
    enabled: true,
  },
  {
    key: "2",
    target: "一厂.清水池平均液位",
    expression: "AVG(TAG('TANK.L1'), TAG('TANK.L2'))",
    source: "TANK.L1, TANK.L2",
    value: "4.32 m",
    enabled: true,
  },
  {
    key: "3",
    target: "二厂.高压泵联动",
    expression: "LINK('一厂.高压泵运行')",
    source: "一厂.高压泵运行",
    value: "运行",
    enabled: true,
  },
];

export default function AdvancedTagDemo() {
  const { message } = App.useApp();
  const [items, setItems] = useState(initialItems);
  const [keyword, setKeyword] = useState("");
  const [editingItem, setEditingItem] = useState<TagLink | null>(null);
  const [modalOpen, setModalOpen] = useState(false);
  const [form] = Form.useForm();

  const filteredItems = items.filter((item) =>
    `${item.target} ${item.expression} ${item.source}`.toLowerCase().includes(keyword.toLowerCase())
  );

  const openModal = (item?: TagLink) => {
    setEditingItem(item || null);
    form.setFieldsValue(
      item || { target: "", expression: "", source: "", value: "--", enabled: true }
    );
    setModalOpen(true);
  };

  const saveItem = async () => {
    const values = await form.validateFields();
    if (editingItem) {
      setItems((current) =>
        current.map((item) => (item.key === editingItem.key ? { ...item, ...values } : item))
      );
      message.success("高级标签已更新（Mock）");
    } else {
      setItems((current) => [...current, { key: `link-${Date.now()}`, ...values }]);
      message.success("高级标签已新增（Mock）");
    }
    setModalOpen(false);
  };

  const columns: ColumnsType<TagLink> = [
    { title: "目标标签", dataIndex: "target", render: (value) => <Text strong>{value}</Text> },
    {
      title: "数学运算 / Link",
      dataIndex: "expression",
      render: (value) => <Text code>{value}</Text>,
    },
    { title: "源标签", dataIndex: "source" },
    {
      title: "实时值",
      dataIndex: "value",
      width: 140,
      render: (value) => <Tag color="processing">{value}</Tag>,
    },
    {
      title: "启用",
      width: 80,
      render: (_, record) => (
        <Switch
          size="small"
          checked={record.enabled}
          onChange={(checked) =>
            setItems((current) =>
              current.map((item) =>
                item.key === record.key ? { ...item, enabled: checked } : item
              )
            )
          }
        />
      ),
    },
    {
      title: "操作",
      width: 130,
      render: (_, record) => (
        <Space>
          <Button type="link" size="small" onClick={() => openModal(record)}>
            编辑
          </Button>
          <Popconfirm
            title="删除这个高级标签？"
            onConfirm={() => {
              setItems((current) => current.filter((item) => item.key !== record.key));
              message.success("高级标签已删除（Mock）");
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
      <div className="flex flex-wrap items-center justify-between gap-4 mb-4">
        <div>
          <Title level={3} className="!mb-1">
            <LinkOutlined /> 高级标签
          </Title>
          <Text type="secondary">标签数学逻辑运算、机器对机器（M2M）标签链接与 Tag Link</Text>
        </div>
        <Space>
          <Input.Search
            allowClear
            value={keyword}
            placeholder="查询标签"
            className="w-48"
            onChange={(event) => setKeyword(event.target.value)}
            onSearch={setKeyword}
          />
          <Button icon={<CheckOutlined />} onClick={() => message.success("全部表达式校验通过")}>
            校验表达式
          </Button>
          <Button type="primary" icon={<PlusOutlined />} onClick={() => openModal()}>
            新建标签 Link
          </Button>
        </Space>
      </div>
      <Card>
        <Table
          columns={columns}
          dataSource={filteredItems}
          pagination={{ pageSize: 8 }}
          scroll={{ x: 1000 }}
        />
      </Card>

      <Modal
        title={editingItem ? "编辑高级标签" : "新建高级标签 Link"}
        open={modalOpen}
        onOk={saveItem}
        onCancel={() => setModalOpen(false)}
        okText="保存"
        cancelText="取消"
      >
        <Form form={form} layout="vertical" className="mt-4">
          <Form.Item name="target" label="目标标签" rules={[{ required: true }]}>
            <Input placeholder="一厂.出水瞬时流量" />
          </Form.Item>
          <Form.Item name="source" label="源标签" rules={[{ required: true }]}>
            <Input placeholder="PLC1.FLOW_OUT" />
          </Form.Item>
          <Form.Item name="expression" label="数学运算 / Link 表达式" rules={[{ required: true }]}>
            <Input.TextArea rows={3} placeholder="TAG('PLC1.FLOW_OUT') * 0.1" />
          </Form.Item>
          <Form.Item name="value" label="模拟实时值" rules={[{ required: true }]}>
            <Input placeholder="426.8 m³/h" />
          </Form.Item>
          <Form.Item name="enabled" label="启用" valuePropName="checked">
            <Switch />
          </Form.Item>
        </Form>
      </Modal>
    </PageContainer>
  );
}
