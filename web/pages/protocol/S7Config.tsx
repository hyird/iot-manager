/**
 * S7 协议配置页面
 * 布局：左侧配置列表 + 右侧连接/轮询区域编辑
 */

import { PlusOutlined } from "@ant-design/icons";
import {
  Button,
  Card,
  Empty,
  Flex,
  Form,
  Input,
  InputNumber,
  Modal,
  Popconfirm,
  Result,
  Select,
  Skeleton,
  Space,
  Switch,
  Table,
  Tag,
  Tooltip,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { useEffect, useMemo, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission, useProtocolImportExport } from "@/hooks";
import { useProtocolConfigDelete, useProtocolConfigList, useProtocolConfigSave } from "@/services";
import type { Protocol, S7 } from "@/types";

type DraftItem = Protocol.Item & { config: S7.Config };

const defaultConfig = (): S7.Config => ({
  connection: {
    host: "",
    rack: 0,
    slot: 1,
    connectionType: "PG",
  },
  pollInterval: 5,
  areas: [],
});

const cloneDraft = (item: Protocol.Item): DraftItem => ({
  ...item,
  config: {
    connection: {
      host: (item.config as S7.Config)?.connection?.host ?? "",
      rack: (item.config as S7.Config)?.connection?.rack ?? 0,
      slot: (item.config as S7.Config)?.connection?.slot ?? 1,
      connectionType: (item.config as S7.Config)?.connection?.connectionType ?? "PG",
    },
    pollInterval: (item.config as S7.Config)?.pollInterval ?? 5,
    areas: ((item.config as S7.Config)?.areas ?? []).map((area) => ({ ...area })),
  },
});

const areaTypeOptions: { value: S7.AreaType; label: string }[] = [
  { value: "DB", label: "DB" },
  { value: "MK", label: "MK" },
  { value: "PE", label: "PE" },
  { value: "PA", label: "PA" },
  { value: "CT", label: "CT" },
  { value: "TM", label: "TM" },
];

const connectionTypeOptions: { value: S7.ConnectionType; label: string }[] = [
  { value: "PG", label: "PG" },
  { value: "OP", label: "OP" },
  { value: "S7_BASIC", label: "S7 Basic" },
];

interface AreaModalProps {
  open: boolean;
  mode: "create" | "edit";
  initialValue?: S7.Area;
  onCancel: () => void;
  onSubmit: (value: S7.Area) => void;
}

function AreaModal({ open, mode, initialValue, onCancel, onSubmit }: AreaModalProps) {
  const [form] = Form.useForm<S7.Area>();

  const handleOk = async () => {
    const values = await form.validateFields();
    onSubmit(values);
  };

  return (
    <Modal
      title={mode === "create" ? "新增区域" : "编辑区域"}
      open={open}
      onCancel={onCancel}
      onOk={handleOk}
      destroyOnHidden
    >
      <Form
        form={form}
        layout="vertical"
        initialValues={
          initialValue ?? {
            id: "",
            name: "",
            area: "DB",
            dbNumber: 1,
            start: 0,
            size: 1,
            writable: false,
            remark: "",
          }
        }
      >
        <Form.Item name="id" label="区域 ID" rules={[{ required: true, message: "请输入区域 ID" }]}>
          <Input placeholder="例如: db_temp" />
        </Form.Item>
        <Form.Item
          name="name"
          label="区域名称"
          rules={[{ required: true, message: "请输入区域名称" }]}
        >
          <Input placeholder="例如: 温度区" />
        </Form.Item>
        <Form.Item name="area" label="区域类型" rules={[{ required: true }]}>
          <Select options={areaTypeOptions} />
        </Form.Item>
        <Form.Item name="dbNumber" label="DB 号">
          <InputNumber min={0} className="w-full" />
        </Form.Item>
        <Space size="middle" className="w-full">
          <Form.Item name="start" label="起始地址" rules={[{ required: true }]} className="flex-1">
            <InputNumber min={0} className="w-full" />
          </Form.Item>
          <Form.Item name="size" label="长度" rules={[{ required: true }]} className="flex-1">
            <InputNumber min={1} className="w-full" />
          </Form.Item>
        </Space>
        <Form.Item name="writable" label="可写" valuePropName="checked">
          <Switch />
        </Form.Item>
        <Form.Item name="remark" label="备注">
          <Input.TextArea rows={3} placeholder="备注" />
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
    isLoading,
    refetch,
  } = useProtocolConfigList({ protocol: "S7" }, { enabled: canQuery });
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();
  const { exportConfigs, triggerImport, importing } = useProtocolImportExport("S7");

  const [selectedTypeId, setSelectedTypeId] = useState<number>();
  const [draft, setDraft] = useState<DraftItem | null>(null);
  const [createModalOpen, setCreateModalOpen] = useState(false);
  const [createForm] = Form.useForm<{ name: string }>();

  const [areaModalOpen, setAreaModalOpen] = useState(false);
  const [editingAreaId, setEditingAreaId] = useState<string | null>(null);

  const types = useMemo(() => configPage?.list || [], [configPage?.list]);

  const activeTypeId = useMemo(() => {
    if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
      return selectedTypeId;
    }
    return types.length > 0 ? types[0].id : undefined;
  }, [selectedTypeId, types]);

  const activeType = useMemo(() => types.find((t) => t.id === activeTypeId), [activeTypeId, types]);

  useEffect(() => {
    if (!activeType) {
      setDraft(null);
      return;
    }
    setDraft(cloneDraft(activeType));
  }, [activeType]);

  const updateDraft = (updater: (current: DraftItem) => DraftItem) => {
    setDraft((current) => (current ? updater(current) : current));
  };

  const areaColumns: ColumnsType<S7.Area> = [
    { title: "区域 ID", dataIndex: "id", ellipsis: true },
    { title: "名称", dataIndex: "name", ellipsis: true },
    {
      title: "类型",
      dataIndex: "area",
      render: (value: S7.AreaType) => <Tag color="blue">{value}</Tag>,
    },
    { title: "DB 号", dataIndex: "dbNumber", render: (value?: number) => value ?? "-" },
    { title: "起始", dataIndex: "start" },
    { title: "长度", dataIndex: "size" },
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
              title="确认删除该区域？"
              onConfirm={() => {
                if (!draft) return;
                updateDraft((current) => ({
                  ...current,
                  config: {
                    ...current.config,
                    areas: current.config.areas.filter((item) => item.id !== record.id),
                  },
                }));
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
  };

  const handleSave = async () => {
    if (!draft) return;
    await saveMutation.mutateAsync({
      id: draft.id,
      protocol: "S7",
      name: draft.name,
      enabled: draft.enabled,
      config: draft.config,
      remark: draft.remark,
    });
    await refetch();
  };

  const handleCreate = async () => {
    const values = await createForm.validateFields();
    await saveMutation.mutateAsync({
      protocol: "S7",
      name: values.name,
      enabled: true,
      config: defaultConfig(),
    });
    setCreateModalOpen(false);
    createForm.resetFields();
    await refetch();
  };

  const editingArea = draft?.config.areas.find((area) => area.id === editingAreaId);

  return (
    <PageContainer title="S7协议配置">
      <Flex gap="middle" align="stretch" className="h-full">
        <Card
          className="w-[360px] shrink-0"
          title="配置列表"
          extra={
            <Space>
              {canAdd && (
                <Button type="primary" size="small" onClick={() => setCreateModalOpen(true)}>
                  新增配置
                </Button>
              )}
              {canImport && (
                <Button
                  size="small"
                  icon={<PlusOutlined />}
                  onClick={triggerImport}
                  loading={importing}
                >
                  导入
                </Button>
              )}
              {canExport && (
                <Button size="small" onClick={() => exportConfigs(types)}>
                  导出
                </Button>
              )}
            </Space>
          }
        >
          {isLoading ? (
            <Skeleton active paragraph={{ rows: 6 }} />
          ) : types.length ? (
            <Table
              size="small"
              rowKey="id"
              pagination={false}
              dataSource={types}
              rowSelection={undefined}
              columns={[
                { title: "名称", dataIndex: "name", ellipsis: true },
                {
                  title: "状态",
                  dataIndex: "enabled",
                  width: 80,
                  render: (enabled: boolean) =>
                    enabled ? <Tag color="green">启用</Tag> : <Tag>停用</Tag>,
                },
                {
                  title: "操作",
                  width: 90,
                  render: (_, row) => (
                    <Button type="link" size="small" onClick={() => setSelectedTypeId(row.id)}>
                      查看
                    </Button>
                  ),
                },
              ]}
              onRow={(record) => ({
                onClick: () => setSelectedTypeId(record.id),
              })}
            />
          ) : (
            <Empty description="暂无 S7 配置" />
          )}
        </Card>

        <Card className="flex-1 min-w-0" title="配置详情">
          {!draft ? (
            <Result status="info" title="请选择或创建一个 S7 配置" />
          ) : (
            <Space direction="vertical" className="w-full" size="large">
              <Space wrap>
                <Input
                  value={draft.name}
                  style={{ width: 280 }}
                  disabled={!canEdit}
                  onChange={(e) => setDraft({ ...draft, name: e.target.value })}
                />
                <Space>
                  <span>启用</span>
                  <Switch
                    checked={draft.enabled}
                    disabled={!canEdit}
                    onChange={(checked) => setDraft({ ...draft, enabled: checked })}
                  />
                </Space>
                <Input
                  value={draft.remark}
                  placeholder="备注"
                  style={{ width: 320 }}
                  disabled={!canEdit}
                  onChange={(e) => setDraft({ ...draft, remark: e.target.value })}
                />
                {canDelete && (
                  <Button danger onClick={handleDeleteType}>
                    删除配置
                  </Button>
                )}
                {canEdit && (
                  <Button type="primary" onClick={handleSave} loading={saveMutation.isPending}>
                    保存配置
                  </Button>
                )}
              </Space>

              <Card size="small" title="连接参数">
                <Space direction="vertical" className="w-full">
                  <Space wrap>
                    <Input
                      value={draft.config.connection.host}
                      disabled={!canEdit}
                      placeholder="PLC 地址"
                      style={{ width: 240 }}
                      onChange={(e) =>
                        updateDraft((current) => ({
                          ...current,
                          config: {
                            ...current.config,
                            connection: { ...current.config.connection, host: e.target.value },
                          },
                        }))
                      }
                    />
                    <InputNumber
                      min={0}
                      value={draft.config.connection.rack}
                      disabled={!canEdit}
                      addonBefore="Rack"
                      onChange={(value) =>
                        updateDraft((current) => ({
                          ...current,
                          config: {
                            ...current.config,
                            connection: { ...current.config.connection, rack: Number(value ?? 0) },
                          },
                        }))
                      }
                    />
                    <InputNumber
                      min={0}
                      value={draft.config.connection.slot}
                      disabled={!canEdit}
                      addonBefore="Slot"
                      onChange={(value) =>
                        updateDraft((current) => ({
                          ...current,
                          config: {
                            ...current.config,
                            connection: { ...current.config.connection, slot: Number(value ?? 0) },
                          },
                        }))
                      }
                    />
                    <Select
                      value={draft.config.connection.connectionType}
                      disabled={!canEdit}
                      style={{ width: 160 }}
                      options={connectionTypeOptions}
                      onChange={(value) =>
                        updateDraft((current) => ({
                          ...current,
                          config: {
                            ...current.config,
                            connection: { ...current.config.connection, connectionType: value },
                          },
                        }))
                      }
                    />
                    <InputNumber
                      min={1}
                      value={draft.config.pollInterval}
                      disabled={!canEdit}
                      addonBefore="轮询(秒)"
                      onChange={(value) =>
                        updateDraft((current) => ({
                          ...current,
                          config: { ...current.config, pollInterval: Number(value ?? 5) },
                        }))
                      }
                    />
                  </Space>
                  <Tooltip title="S7 适配器会按这个间隔轮询区域并回传原始字节数据">
                    <Tag color="blue">areas={draft.config.areas.length}</Tag>
                  </Tooltip>
                </Space>
              </Card>

              <Card
                size="small"
                title="轮询区域"
                extra={
                  canEdit ? (
                    <Button
                      size="small"
                      type="primary"
                      onClick={() => {
                        setEditingAreaId(null);
                        setAreaModalOpen(true);
                      }}
                    >
                      新增区域
                    </Button>
                  ) : null
                }
              >
                <Table
                  size="small"
                  rowKey="id"
                  pagination={false}
                  dataSource={draft.config.areas}
                  columns={areaColumns}
                  locale={{ emptyText: <Empty description="暂无区域" /> }}
                />
              </Card>
            </Space>
          )}
        </Card>
      </Flex>

      <Modal
        title="新增S7配置"
        open={createModalOpen}
        onCancel={() => setCreateModalOpen(false)}
        onOk={handleCreate}
        destroyOnHidden
      >
        <Form form={createForm} layout="vertical" initialValues={{ name: "新建S7配置" }}>
          <Form.Item
            name="name"
            label="配置名称"
            rules={[{ required: true, message: "请输入配置名称" }]}
          >
            <Input placeholder="例如: PLC-A" />
          </Form.Item>
        </Form>
      </Modal>

      <AreaModal
        open={areaModalOpen}
        mode={editingArea ? "edit" : "create"}
        initialValue={editingArea}
        onCancel={() => setAreaModalOpen(false)}
        onSubmit={(value) => {
          updateDraft((current) => {
            const areas = current.config.areas.some((area) => area.id === value.id)
              ? current.config.areas.map((area) => (area.id === value.id ? value : area))
              : [...current.config.areas, value];
            return { ...current, config: { ...current.config, areas } };
          });
          setAreaModalOpen(false);
          setEditingAreaId(null);
        }}
      />
    </PageContainer>
  );
};

export default S7ConfigPage;
