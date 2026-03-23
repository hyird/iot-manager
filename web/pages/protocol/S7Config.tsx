/**
 * S7 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
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

type ConnectionFormValues = {
  deviceType: string;
  plcModel: S7.PlcModel;
  host: string;
  rack: number;
  slot: number;
  connectionType: S7.ConnectionType;
  pollInterval: number;
};

const defaultConfig = (): S7.Config => ({
  deviceType: "",
  plcModel: "S7-1200",
  connection: {
    host: "",
    rack: 0,
    slot: 1,
    connectionType: "PG",
  },
  pollInterval: 5,
  areas: [],
});

const plcModelOptions: { value: S7.PlcModel; label: string; rack: number; slot: number }[] = [
  { value: "S7-300", label: "S7-300", rack: 0, slot: 2 },
  { value: "S7-400", label: "S7-400", rack: 0, slot: 3 },
  { value: "S7-1200", label: "S7-1200", rack: 0, slot: 1 },
  { value: "S7-1500", label: "S7-1500", rack: 0, slot: 1 },
];

const getPlcPreset = (plcModel: S7.PlcModel) =>
  plcModelOptions.find((option) => option.value === plcModel) ?? plcModelOptions[2];

const cloneDraft = (item: Protocol.Item): DraftItem => ({
  ...item,
  config: {
    deviceType: (item.config as S7.Config)?.deviceType ?? "",
    plcModel: (item.config as S7.Config)?.plcModel ?? "S7-1200",
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
      title={mode === "create" ? "新增寄存器" : "编辑寄存器"}
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
        <Form.Item
          name="id"
          label="寄存器 ID"
          rules={[{ required: true, message: "请输入寄存器 ID" }]}
        >
          <Input placeholder="例如: reg_temp" />
        </Form.Item>
        <Form.Item
          name="name"
          label="寄存器名称"
          rules={[{ required: true, message: "请输入寄存器名称" }]}
        >
          <Input placeholder="例如: 温度寄存器" />
        </Form.Item>
        <Form.Item name="area" label="寄存器类型" rules={[{ required: true }]}>
          <Select options={areaTypeOptions} />
        </Form.Item>
        <Form.Item name="dbNumber" label="DB 编号">
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
  const [createForm] = Form.useForm<{ deviceType: string; plcModel: S7.PlcModel }>();
  const [connectionForm] = Form.useForm<ConnectionFormValues>();
  const watchedCreatePlcModel = Form.useWatch("plcModel", createForm) ?? "S7-1200";

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

  useEffect(() => {
    if (!draft) return;
    connectionForm.setFieldsValue({
      deviceType: draft.config.deviceType,
      plcModel: draft.config.plcModel,
      host: draft.config.connection.host,
      rack: draft.config.connection.rack,
      slot: draft.config.connection.slot,
      connectionType: draft.config.connection.connectionType,
      pollInterval: draft.config.pollInterval ?? 5,
    });
  }, [activeTypeId, connectionForm]);

  useEffect(() => {
    if (!draft) return;
    const preset = getPlcPreset(draft.config.plcModel);
    if (
      draft.config.connection.rack !== preset.rack ||
      draft.config.connection.slot !== preset.slot
    ) {
      updateDraft((current) => ({
        ...current,
        config: {
          ...current.config,
          connection: {
            ...current.config.connection,
            rack: preset.rack,
            slot: preset.slot,
          },
        },
      }));
    }
  }, [draft?.config.plcModel]);

  const updateDraft = (updater: (current: DraftItem) => DraftItem) => {
    setDraft((current) => (current ? updater(current) : current));
  };

  const areaColumns: ColumnsType<S7.Area> = [
    { title: "寄存器 ID", dataIndex: "id", ellipsis: true },
    { title: "寄存器名称", dataIndex: "name", ellipsis: true },
    {
      title: "寄存器类型",
      dataIndex: "area",
      render: (value: S7.AreaType) => <Tag color="blue">{value}</Tag>,
    },
    { title: "DB 编号", dataIndex: "dbNumber", render: (value?: number) => value ?? "-" },
    { title: "起始地址", dataIndex: "start" },
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
              title="确认删除该寄存器？"
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
    await connectionForm.validateFields();
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
    const preset = getPlcPreset(values.plcModel);
    await saveMutation.mutateAsync({
      protocol: "S7",
      name: values.deviceType,
      enabled: true,
      config: {
        ...defaultConfig(),
        deviceType: values.deviceType,
        plcModel: values.plcModel,
        connection: {
          ...defaultConfig().connection,
          rack: preset.rack,
          slot: preset.slot,
        },
      },
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
          title="设备类型"
          extra={
            <Space>
              {canAdd && (
                <Button type="primary" size="small" onClick={() => setCreateModalOpen(true)}>
                  新建设备类型
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
                {
                  title: "设备类型",
                  dataIndex: "name",
                  ellipsis: true,
                  render: (_, row) => (row.config as S7.Config)?.deviceType || row.name,
                },
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

        <Card className="flex-1 min-w-0" title="寄存器配置">
          {!draft ? (
            <Result status="info" title="请选择或创建一个 S7 配置" />
          ) : (
            <Space direction="vertical" className="w-full" size="large">
              <Space wrap>
                <Input
                  value={draft.config.deviceType || draft.name}
                  style={{ width: 280 }}
                  disabled={!canEdit}
                  onChange={(e) =>
                    setDraft({
                      ...draft,
                      name: e.target.value,
                      config: { ...draft.config, deviceType: e.target.value },
                    })
                  }
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
                <Form
                  form={connectionForm}
                  layout="vertical"
                  disabled={!canEdit}
                  onValuesChange={(_, values) =>
                    updateDraft((current) => ({
                      ...current,
                      config: {
                        ...current.config,
                        deviceType: values.deviceType,
                        plcModel: values.plcModel,
                        connection: {
                          ...current.config.connection,
                          host: values.host,
                          rack: values.rack,
                          slot: values.slot,
                          connectionType: values.connectionType,
                        },
                        pollInterval: values.pollInterval,
                      },
                    }))
                  }
                >
                  <Space direction="vertical" className="w-full" size="middle">
                    <Form.Item
                      name="deviceType"
                      label="设备类型"
                      rules={[{ required: true, message: "请输入设备类型" }]}
                    >
                      <Input placeholder="例如：S7-1200" />
                    </Form.Item>
                    <Form.Item
                      name="plcModel"
                      label="PLC型号"
                      rules={[{ required: true, message: "请选择PLC型号" }]}
                    >
                      <Select
                        options={plcModelOptions.map(({ value, label }) => ({ value, label }))}
                        onChange={(value: S7.PlcModel) => {
                          const preset = getPlcPreset(value);
                          connectionForm.setFieldsValue({ rack: preset.rack, slot: preset.slot });
                          updateDraft((current) => ({
                            ...current,
                            config: {
                              ...current.config,
                              plcModel: value,
                              connection: {
                                ...current.config.connection,
                                rack: preset.rack,
                                slot: preset.slot,
                              },
                            },
                          }));
                        }}
                      />
                    </Form.Item>
                    <Space wrap className="w-full">
                      <Form.Item
                        name="host"
                        label="PLC 地址"
                        rules={[{ required: true, message: "S7配置的 connection.host 不能为空" }]}
                        className="flex-1 min-w-[240px]"
                      >
                        <Input placeholder="PLC 地址" />
                      </Form.Item>
                      <Form.Item name="rack" label="Rack">
                        <InputNumber min={0} className="w-full" disabled />
                      </Form.Item>
                      <Form.Item name="slot" label="Slot">
                        <InputNumber min={0} className="w-full" disabled />
                      </Form.Item>
                      <Form.Item name="connectionType" label="连接类型" className="min-w-[160px]">
                        <Select options={connectionTypeOptions} />
                      </Form.Item>
                      <Form.Item name="pollInterval" label="轮询(秒)">
                        <InputNumber min={1} className="w-full" />
                      </Form.Item>
                    </Space>
                    <Tooltip title="S7 适配器会按这个间隔轮询区域并回传原始字节数据">
                      <Tag color="blue">registers={draft.config.areas.length}</Tag>
                    </Tooltip>
                  </Space>
                </Form>
              </Card>

              <Card
                size="small"
                title="寄存器配置"
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
                      新增寄存器
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
                  locale={{ emptyText: <Empty description="暂无寄存器" /> }}
                />
              </Card>
            </Space>
          )}
        </Card>
      </Flex>

      <Modal
        title="新建设备类型"
        open={createModalOpen}
        onCancel={() => setCreateModalOpen(false)}
        onOk={handleCreate}
        destroyOnHidden
      >
        <Form
          form={createForm}
          layout="vertical"
          initialValues={{ deviceType: "", plcModel: "S7-1200" }}
        >
          <Form.Item
            name="deviceType"
            label="设备类型"
            rules={[{ required: true, message: "请输入设备类型" }]}
          >
            <Input placeholder="例如: S7-1200" />
          </Form.Item>
          <Form.Item
            name="plcModel"
            label="PLC型号"
            rules={[{ required: true, message: "请选择PLC型号" }]}
          >
            <Select
              options={plcModelOptions.map(({ value, label }) => ({ value, label }))}
              onChange={(value: S7.PlcModel) => {
                const preset = getPlcPreset(value);
                connectionForm.setFieldsValue({ rack: preset.rack, slot: preset.slot });
              }}
            />
          </Form.Item>
          <Space size="middle" className="w-full">
            <Form.Item label="Rack" className="flex-1">
              <InputNumber
                value={getPlcPreset(watchedCreatePlcModel).rack}
                disabled
                className="w-full"
              />
            </Form.Item>
            <Form.Item label="Slot" className="flex-1">
              <InputNumber
                value={getPlcPreset(watchedCreatePlcModel).slot}
                disabled
                className="w-full"
              />
            </Form.Item>
          </Space>
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
