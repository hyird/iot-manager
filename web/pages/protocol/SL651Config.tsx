/**
 * SL651 协议配置页面
 * 参考原项目 iot-frontend/src/views/SL651/TypeConfig
 * 布局：左侧设备类型列表 + 右侧功能码/要素配置
 */

import { forwardRef, useImperativeHandle, useMemo, useRef, useState } from "react";
import {
  Layout,
  Card,
  Tree,
  Tag,
  Flex,
  Button,
  Space,
  Table,
  Popconfirm,
  Empty,
  Skeleton,
  Modal,
  Form,
  Input,
  InputNumber,
  Select,
  Switch,
  Result,
  Tooltip,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import type { Protocol, SL651 } from "@/types";
import { usePermission } from "@/hooks";
import { PageContainer } from "@/components/PageContainer";
import { useProtocolConfigList, useProtocolConfigSave, useProtocolConfigDelete } from "@/services";

const { Sider, Content } = Layout;

/** 编码类型列表 */
const EncodeList: SL651.EncodeType[] = ["BCD", "TIME_YYMMDDHHMMSS", "JPEG", "DICT", "HEX"];

/** 生成唯一 ID */
const generateId = () => crypto.randomUUID();

/** 设备类型 Modal Ref */
interface DeviceTypeModalRef {
  open: (mode: "create" | "edit", data?: Protocol.Item) => void;
}

/** 功能码 Modal Ref */
interface FuncModalRef {
  open: (mode: "create" | "edit", typeId: number, func?: SL651.Func) => void;
}

/** 要素 Modal Ref */
interface ElementModalRef {
  open: (mode: "create" | "edit", typeId: number, funcId: string, element?: SL651.Element) => void;
}

/** 预设值 Modal Ref */
interface PresetValueModalRef {
  open: (typeId: number, funcId: string, element: SL651.Element) => void;
}

/** 字典配置 Modal Ref */
interface DictConfigModalRef {
  open: (typeId: number, funcId: string, element: SL651.Element) => void;
}

/** 带要素的功能码 */
interface FuncWithElements extends SL651.Func {
  elements: SL651.Element[];
}

/** 表单中的条件数据（可能不完整） */
interface FormCondition {
  bitIndex?: string;
  bitValue?: string;
}

/** 表单中的映射项数据（可能不完整） */
interface FormMapItem {
  key?: string;
  label?: string;
  value?: string;
  dependsOn?: {
    operator?: "AND" | "OR";
    conditions?: FormCondition[];
  };
}

const SL651ConfigPage = () => {
  // 权限检查
  const canQuery = usePermission("iot:protocol:query");
  const canAdd = usePermission("iot:protocol:add");
  const canEdit = usePermission("iot:protocol:edit");
  const canDelete = usePermission("iot:protocol:delete");

  // 设备类型列表查询
  const {
    data: configPage,
    isLoading: loadingTypes,
    refetch: refetchTypes,
  } = useProtocolConfigList({ protocol: "SL651" }, { enabled: canQuery });

  // 保存和删除 mutations
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();

  // 当前选中的设备类型 ID（用户手动选择）
  const [selectedTypeId, setSelectedTypeId] = useState<number>();

  // Modal refs
  const deviceTypeModalRef = useRef<DeviceTypeModalRef>(null);
  const funcModalRef = useRef<FuncModalRef>(null);
  const elementModalRef = useRef<ElementModalRef>(null);
  const presetValueModalRef = useRef<PresetValueModalRef>(null);
  const dictConfigModalRef = useRef<DictConfigModalRef>(null);

  // 设备类型列表（使用 useMemo 保持引用稳定）
  const types = useMemo(() => configPage?.list || [], [configPage?.list]);

  // 计算当前激活的类型 ID：优先用户选择，否则默认第一个
  const activeTypeId = useMemo(() => {
    if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
      return selectedTypeId;
    }
    return types.length > 0 ? types[0].id : undefined;
  }, [selectedTypeId, types]);

  // 功能码列表（派生状态，根据 activeTypeId 计算）
  const funcs = useMemo<FuncWithElements[]>(() => {
    if (!activeTypeId) return [];
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return [];
    const config = type.config as SL651.Config;
    return config?.funcs || [];
  }, [activeTypeId, types]);

  // 加载状态（与数据加载状态同步）
  const loadingFuncs = loadingTypes;

  // ========== 设备类型操作 ==========

  const handleDeleteDeviceType = async () => {
    if (!activeTypeId) return;
    await deleteMutation.mutateAsync(activeTypeId);
    setSelectedTypeId(undefined);
  };

  // ========== 功能码操作 ==========

  const handleDeleteFunc = async (funcId: string) => {
    if (!activeTypeId) return;
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return;

    const config = type.config as SL651.Config;
    const newConfig: SL651.Config = {
      funcs: config.funcs.filter((f) => f.id !== funcId),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "SL651",
      config: newConfig,
    });
  };

  // ========== 要素操作 ==========

  const handleDeleteElement = async (funcId: string, eleId: string) => {
    if (!activeTypeId) return;
    const type = types.find((t) => t.id === activeTypeId);
    if (!type) return;

    const config = type.config as SL651.Config;
    const newConfig: SL651.Config = {
      funcs: config.funcs.map((f) =>
        f.id === funcId ? { ...f, elements: f.elements.filter((e) => e.id !== eleId) } : f
      ),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "SL651",
      config: newConfig,
    });
  };

  // ========== 功能码表格列 ==========

  const funcColumns: ColumnsType<FuncWithElements> = [
    { title: "功能码", dataIndex: "funcCode", width: 100 },
    { title: "名称", dataIndex: "name", width: 150 },
    {
      title: "方向",
      width: 100,
      render: (_, r) => (
        <Tag color={r.dir === "UP" ? "green" : "orange"}>{r.dir === "UP" ? "上行" : "下行"}</Tag>
      ),
    },
    {
      title: "要素数",
      width: 100,
      render: (_, r) => r.elements?.length ?? 0,
    },
    {
      title: "备注",
      dataIndex: "remark",
    },
    {
      title: "操作",
      width: 260,
      render: (_, r) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => funcModalRef.current?.open("edit", activeTypeId!, r)}
            >
              编辑
            </Button>
          )}
          {canDelete && (
            <Popconfirm title="确认删除？" onConfirm={() => handleDeleteFunc(r.id)}>
              <Button size="small" danger type="link">
                删除
              </Button>
            </Popconfirm>
          )}
          {canAdd && (
            <Button
              size="small"
              type="link"
              onClick={() => elementModalRef.current?.open("create", activeTypeId!, r.id)}
            >
              新增要素
            </Button>
          )}
        </Space>
      ),
    },
  ];

  // ========== 要素表格列 ==========

  const elementColumns = (funcId: string, funcDir: SL651.Direction): ColumnsType<SL651.Element> => [
    { title: "要素名称", dataIndex: "name", width: 150 },
    { title: "引导符", dataIndex: "guideHex", width: 100 },
    { title: "编码", dataIndex: "encode", width: 100 },
    { title: "长度", dataIndex: "length", width: 80 },
    { title: "小数位", dataIndex: "digits", width: 80 },
    { title: "单位", dataIndex: "unit", width: 80 },
    ...(funcDir === "DOWN"
      ? [
          {
            title: "预设值",
            dataIndex: "options",
            width: 100,
            render: (options: SL651.Element["options"]) =>
              options?.length ? `${options.length}个` : "-",
          },
        ]
      : []),
    {
      title: "字典配置",
      dataIndex: "dictConfig",
      width: 100,
      render: (dictConfig: SL651.Element["dictConfig"]) => {
        if (!dictConfig?.items?.length) return "-";
        const typeLabel = dictConfig.mapType === "VALUE" ? "值" : "位";
        return `${typeLabel}映射(${dictConfig.items.length}个)`;
      },
    },
    { title: "备注", dataIndex: "remark", width: 120 },
    {
      title: "操作",
      width: funcDir === "DOWN" ? 300 : 240,
      render: (_, r) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => elementModalRef.current?.open("edit", activeTypeId!, funcId, r)}
            >
              编辑
            </Button>
          )}
          {funcDir === "DOWN" && canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => presetValueModalRef.current?.open(activeTypeId!, funcId, r)}
            >
              预设值
            </Button>
          )}
          {r.encode === "DICT" && canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => dictConfigModalRef.current?.open(activeTypeId!, funcId, r)}
            >
              字典
            </Button>
          )}
          {canDelete && (
            <Popconfirm title="确认删除？" onConfirm={() => handleDeleteElement(funcId, r.id)}>
              <Button size="small" danger type="link">
                删除
              </Button>
            </Popconfirm>
          )}
        </Space>
      ),
    },
  ];

  // 权限检查
  if (!canQuery) {
    return (
      <PageContainer title="SL651配置">
        <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
      </PageContainer>
    );
  }

  return (
    <PageContainer title="SL651配置">
      <Layout style={{ height: "100%", background: "transparent" }}>
        {/* 左侧：设备类型列表 */}
        <Sider width={360} style={{ background: "transparent", height: "100%", paddingRight: 12 }}>
          <Card
            title="设备类型"
            style={{ height: "100%", display: "flex", flexDirection: "column" }}
            styles={{ body: { flex: 1, overflow: "auto", padding: 16 } }}
            extra={
              <Space>
                {canAdd && (
                  <Button type="primary" onClick={() => deviceTypeModalRef.current?.open("create")}>
                    新增
                  </Button>
                )}
                {canEdit && (
                  <Button
                    disabled={!activeTypeId}
                    onClick={() =>
                      deviceTypeModalRef.current?.open(
                        "edit",
                        types.find((t) => t.id === activeTypeId)
                      )
                    }
                  >
                    编辑
                  </Button>
                )}
                {canDelete && (
                  <Popconfirm
                    title="确认删除该设备类型？"
                    onConfirm={handleDeleteDeviceType}
                    disabled={!activeTypeId}
                  >
                    <Button danger disabled={!activeTypeId}>
                      删除
                    </Button>
                  </Popconfirm>
                )}
              </Space>
            }
          >
            {loadingTypes ? (
              <Skeleton active paragraph={{ rows: 6 }} />
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
                  const config = t.config as SL651.Config;
                  const mode = config?.responseMode || "M1";
                  return {
                    key: String(t.id),
                    title: (
                      <Tooltip title={t.remark || "暂无备注"} placement="right">
                        <Flex
                          justify="space-between"
                          align="center"
                          style={{ height: 32, padding: 4 }}
                        >
                          <Space size={4}>
                            <span>{t.name}</span>
                            <Tag color="blue">{mode}</Tag>
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
        </Sider>

        {/* 右侧：功能码配置 */}
        <Content style={{ height: "100%" }}>
          <Card
            title={activeTypeId ? "功能码配置" : "请选择设备类型"}
            style={{ height: "100%", display: "flex", flexDirection: "column" }}
            styles={{ body: { flex: 1, overflow: "auto", padding: 0 } }}
            extra={
              activeTypeId &&
              canAdd && (
                <Button
                  type="primary"
                  onClick={() => funcModalRef.current?.open("create", activeTypeId)}
                >
                  新增功能码
                </Button>
              )
            }
          >
            {!activeTypeId ? (
              <Empty description="未选择设备类型" />
            ) : (
              <div style={{ "--ant-table-header-border-radius": 0 } as React.CSSProperties}>
                <Table
                  dataSource={funcs}
                  rowKey="id"
                  pagination={false}
                  loading={loadingFuncs}
                  sticky
                  expandable={{
                    expandedRowRender: (record) => (
                      <Table
                        dataSource={record.elements}
                        columns={elementColumns(record.id, record.dir)}
                        rowKey="id"
                        pagination={false}
                        size="small"
                      />
                    ),
                  }}
                  columns={funcColumns}
                />
              </div>
            )}
          </Card>
        </Content>
      </Layout>

      {/* 设备类型 Modal */}
      <DeviceTypeModal
        ref={deviceTypeModalRef}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 功能码 Modal */}
      <FuncModal
        ref={funcModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 要素 Modal */}
      <ElementModal
        ref={elementModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 预设值 Modal */}
      <PresetValueModal
        ref={presetValueModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />

      {/* 字典配置 Modal */}
      <DictConfigModal
        ref={dictConfigModalRef}
        types={types}
        onSuccess={refetchTypes}
        saveMutation={saveMutation}
      />
    </PageContainer>
  );
};

// ========== 设备类型 Modal ==========

interface DeviceTypeModalProps {
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const DeviceTypeModal = forwardRef<DeviceTypeModalRef, DeviceTypeModalProps>(
  ({ onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [mode, setMode] = useState<"create" | "edit">("create");
    const [current, setCurrent] = useState<Protocol.Item>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(m, data) {
        setMode(m);
        setCurrent(data);
        form.resetFields();
        const config = data?.config as SL651.Config | undefined;
        form.setFieldsValue(
          data
            ? {
                name: data.name,
                enabled: data.enabled,
                responseMode: config?.responseMode || "M1",
                remark: data.remark,
              }
            : { enabled: true, responseMode: "M1" }
        );
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      const values = await form.validateFields();
      const existingConfig = (current?.config as SL651.Config) || { funcs: [] };

      await saveMutation.mutateAsync({
        id: current?.id,
        protocol: "SL651",
        name: values.name,
        enabled: values.enabled,
        config: { ...existingConfig, responseMode: values.responseMode },
        remark: values.remark,
      });

      onSuccess?.();
      setOpen(false);
    };

    return (
      <Modal
        title={mode === "create" ? "新增设备类型" : "编辑设备类型"}
        open={open}
        onOk={handleOk}
        onCancel={() => setOpen(false)}
        confirmLoading={saveMutation.isPending}
        forceRender
      >
        <Form form={form} layout="vertical">
          <Form.Item label="名称" name="name" rules={[{ required: true, message: "请输入名称" }]}>
            <Input />
          </Form.Item>
          <Form.Item
            label="应答模式"
            name="responseMode"
            rules={[{ required: true, message: "请选择应答模式" }]}
          >
            <Select
              options={[
                { value: "M1", label: "M1 - 自报" },
                { value: "M2", label: "M2 - 自报/查询应答兼容" },
                { value: "M3", label: "M3 - 查询应答" },
                { value: "M4", label: "M4 - 调试/召测" },
              ]}
            />
          </Form.Item>
          <Form.Item label="启用" name="enabled" valuePropName="checked">
            <Switch />
          </Form.Item>
          <Form.Item label="备注" name="remark">
            <Input.TextArea rows={3} />
          </Form.Item>
        </Form>
      </Modal>
    );
  }
);

// ========== 功能码 Modal ==========

interface FuncModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const FuncModal = forwardRef<FuncModalRef, FuncModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [mode, setMode] = useState<"create" | "edit">("create");
    const [typeId, setTypeId] = useState<number>();
    const [current, setCurrent] = useState<SL651.Func>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(m, t, func) {
        setMode(m);
        setTypeId(t);
        setCurrent(func);
        form.resetFields();
        form.setFieldsValue(func ?? { dir: "UP" });
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      if (!typeId) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      const config = type.config as SL651.Config;
      let newFuncs: SL651.Func[];

      if (mode === "create") {
        const newFunc: SL651.Func = {
          id: generateId(),
          funcCode: values.funcCode,
          dir: values.dir,
          name: values.name,
          remark: values.remark,
          elements: [],
        };
        newFuncs = [...(config.funcs || []), newFunc];
      } else {
        newFuncs = config.funcs.map((f) =>
          f.id === current?.id
            ? {
                ...f,
                funcCode: values.funcCode,
                dir: values.dir,
                name: values.name,
                remark: values.remark,
              }
            : f
        );
      }

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "SL651",
        config: { funcs: newFuncs },
      });

      onSuccess?.();
      setOpen(false);
    };

    return (
      <Modal
        title={mode === "create" ? "新增功能码" : "编辑功能码"}
        open={open}
        onOk={handleOk}
        onCancel={() => setOpen(false)}
        confirmLoading={saveMutation.isPending}
        forceRender
      >
        <Form form={form} layout="vertical">
          <Form.Item label="名称" name="name" rules={[{ required: true, message: "请输入名称" }]}>
            <Input />
          </Form.Item>
          <Form.Item
            label="功能码"
            name="funcCode"
            rules={[{ required: true, message: "请输入功能码" }]}
          >
            <Input placeholder="例如 2F" />
          </Form.Item>
          <Form.Item label="方向" name="dir" rules={[{ required: true, message: "请选择方向" }]}>
            <Select
              options={[
                { value: "UP", label: "上行" },
                { value: "DOWN", label: "下行" },
              ]}
            />
          </Form.Item>
          <Form.Item label="备注" name="remark">
            <Input.TextArea rows={3} />
          </Form.Item>
        </Form>
      </Modal>
    );
  }
);

// ========== 要素 Modal ==========

interface ElementModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const ElementModal = forwardRef<ElementModalRef, ElementModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [mode, setMode] = useState<"create" | "edit">("create");
    const [typeId, setTypeId] = useState<number>();
    const [funcId, setFuncId] = useState<string>();
    const [current, setCurrent] = useState<SL651.Element>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(m, t, fId, element) {
        setMode(m);
        setTypeId(t);
        setFuncId(fId);
        setCurrent(element);
        form.resetFields();
        form.setFieldsValue(element ?? { encode: "BCD", length: 0, digits: 0 });
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      if (!typeId || !funcId) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      const config = type.config as SL651.Config;
      const newFuncs = config.funcs.map((f) => {
        if (f.id !== funcId) return f;

        let newElements: SL651.Element[];
        if (mode === "create") {
          const newElement: SL651.Element = {
            id: generateId(),
            name: values.name,
            guideHex: values.guideHex,
            encode: values.encode,
            length: values.length,
            digits: values.digits,
            unit: values.unit,
            remark: values.remark,
          };
          newElements = [...(f.elements || []), newElement];
        } else {
          newElements = f.elements.map((e) => (e.id === current?.id ? { ...e, ...values } : e));
        }

        return { ...f, elements: newElements };
      });

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "SL651",
        config: { funcs: newFuncs },
      });

      onSuccess?.();
      setOpen(false);
    };

    return (
      <Modal
        open={open}
        title={mode === "create" ? "新增要素" : "编辑要素"}
        onCancel={() => setOpen(false)}
        onOk={handleOk}
        confirmLoading={saveMutation.isPending}
        forceRender
        width={520}
      >
        <Form form={form} layout="vertical">
          <Form.Item
            label="要素名称"
            name="name"
            rules={[{ required: true, message: "请输入名称" }]}
          >
            <Input />
          </Form.Item>
          <Form.Item
            label="引导符（HEX）"
            name="guideHex"
            rules={[{ required: true, message: "请输入引导符" }]}
          >
            <Input placeholder="例如：01 或 F3F3" />
          </Form.Item>
          <Form.Item label="编码" name="encode" rules={[{ required: true, message: "请选择编码" }]}>
            <Select options={EncodeList.map((e) => ({ value: e, label: e }))} />
          </Form.Item>
          <Form.Item label="长度" name="length" rules={[{ required: true, message: "请输入长度" }]}>
            <InputNumber min={0} style={{ width: "100%" }} />
          </Form.Item>
          <Form.Item
            label="小数位"
            name="digits"
            rules={[{ required: true, message: "请输入小数位" }]}
          >
            <InputNumber min={0} max={8} style={{ width: "100%" }} />
          </Form.Item>
          <Form.Item label="单位" name="unit">
            <Input placeholder="例如 V、℃、m³/s" />
          </Form.Item>
          <Form.Item label="备注" name="remark">
            <Input.TextArea rows={2} />
          </Form.Item>
        </Form>
      </Modal>
    );
  }
);

// ========== 预设值 Modal ==========

interface PresetValueModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const PresetValueModal = forwardRef<PresetValueModalRef, PresetValueModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [typeId, setTypeId] = useState<number>();
    const [funcId, setFuncId] = useState<string>();
    const [element, setElement] = useState<SL651.Element>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(t, fId, ele) {
        setTypeId(t);
        setFuncId(fId);
        setElement(ele);
        form.resetFields();
        form.setFieldsValue({ options: ele.options || [] });
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      if (!typeId || !funcId || !element) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      const config = type.config as SL651.Config;
      const newFuncs = config.funcs.map((f) => {
        if (f.id !== funcId) return f;

        const newElements = f.elements.map((e) =>
          e.id === element.id ? { ...e, options: values.options } : e
        );

        return { ...f, elements: newElements };
      });

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "SL651",
        config: { funcs: newFuncs },
      });

      onSuccess?.();
      setOpen(false);
    };

    return (
      <Modal
        open={open}
        title={`配置预设值 - ${element?.name}`}
        onCancel={() => setOpen(false)}
        onOk={handleOk}
        confirmLoading={saveMutation.isPending}
        forceRender
        width={600}
      >
        <Form form={form} layout="vertical">
          <Form.Item label="预设值选项" extra="配置后指令下发时可从下拉列表选择">
            <Form.List name="options">
              {(fields, { add, remove }) => (
                <>
                  {fields.map(({ key, name, ...restField }) => (
                    <Flex key={key} gap={8} align="center" style={{ marginBottom: 8 }}>
                      <Form.Item
                        {...restField}
                        name={[name, "label"]}
                        rules={[{ required: true, message: "请输入名称" }]}
                        style={{ flex: 1, marginBottom: 0 }}
                      >
                        <Input placeholder="显示名称" />
                      </Form.Item>
                      <Form.Item
                        {...restField}
                        name={[name, "value"]}
                        rules={[{ required: true, message: "请输入值" }]}
                        style={{ flex: 1, marginBottom: 0 }}
                      >
                        <Input placeholder="实际值" />
                      </Form.Item>
                      <Button type="text" danger onClick={() => remove(name)}>
                        删除
                      </Button>
                    </Flex>
                  ))}
                  <Button type="dashed" onClick={() => add()} block>
                    + 添加预设值
                  </Button>
                </>
              )}
            </Form.List>
          </Form.Item>
        </Form>
      </Modal>
    );
  }
);

// ========== 字典配置 Modal ==========

interface DictConfigModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const DictConfigModal = forwardRef<DictConfigModalRef, DictConfigModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [typeId, setTypeId] = useState<number>();
    const [funcId, setFuncId] = useState<string>();
    const [element, setElement] = useState<SL651.Element>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(t, fId, ele) {
        setTypeId(t);
        setFuncId(fId);
        setElement(ele);
        setOpen(true);

        // 延迟设置表单值，确保 Modal 已经打开
        setTimeout(() => {
          form.resetFields();

          const mapType = ele.dictConfig?.mapType || "VALUE";

          // ✅ 根据映射类型区分处理数据，过滤并修复不完整的数据
          const items = (ele.dictConfig?.items || [])
            .filter((item) => {
              // 只过滤掉完全无效的项（null、undefined、非对象）
              return item && typeof item === "object";
            })
            .map((item) => {
              if (mapType === "VALUE") {
                // 值映射：只保留 key 和 label，提供默认值
                return {
                  key: item.key || "",
                  label: item.label || "",
                };
              } else {
                // 位映射：保留 key、label、value 和 dependsOn，提供默认值
                // 过滤掉 null 和无效的依赖条件
                const validConditions = (item.dependsOn?.conditions || [])
                  .filter(
                    (c: FormCondition) =>
                      c &&
                      typeof c === "object" &&
                      c.bitIndex !== undefined &&
                      c.bitValue !== undefined
                  )
                  .map((c: FormCondition) => ({
                    bitIndex: String(c.bitIndex),
                    bitValue: String(c.bitValue),
                  }));

                return {
                  key: item.key || "",
                  label: item.label || "",
                  value: item.value || "1",
                  dependsOn: {
                    operator: item.dependsOn?.operator || "AND",
                    conditions: validConditions,
                  },
                };
              }
            });

          form.setFieldsValue({
            mapType,
            items,
          });
        }, 0);
      },
    }));

    const handleOk = async () => {
      if (!typeId || !funcId || !element) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      // ✅ 根据映射类型清理数据，过滤掉空项
      const cleanedItems = (values.items || [])
        .filter((item: FormMapItem) => {
          // 过滤掉 key 和 label 都为空的项
          const key = String(item.key || "").trim();
          const label = String(item.label || "").trim();
          return key !== "" && label !== "";
        })
        .map((item: FormMapItem) => {
          if (values.mapType === "VALUE") {
            // 值映射：只保存 key 和 label
            return {
              key: String(item.key || "").trim(),
              label: String(item.label || "").trim(),
            };
          } else {
            // 位映射：保存 key、label、value
            const cleanedItem: SL651.DictMapItem = {
              key: String(item.key || "").trim(),
              label: String(item.label || "").trim(),
              value: item.value || "1",
            };

            // 过滤掉 null 和无效的依赖条件
            const validConditions = (item.dependsOn?.conditions || [])
              .filter(
                (c: FormCondition) =>
                  c &&
                  typeof c === "object" &&
                  c.bitIndex !== undefined &&
                  c.bitValue !== undefined &&
                  String(c.bitIndex).trim() !== ""
              )
              .map((c: FormCondition) => ({
                bitIndex: String(c.bitIndex).trim(),
                bitValue: String(c.bitValue),
              }));

            // 只在有有效依赖条件时才添加 dependsOn（依赖条件是可选的）
            if (validConditions.length > 0) {
              cleanedItem.dependsOn = {
                operator: item.dependsOn?.operator || "AND",
                conditions: validConditions,
              };
            }

            return cleanedItem;
          }
        });

      const config = type.config as SL651.Config;
      const newFuncs = config.funcs.map((f) => {
        if (f.id !== funcId) return f;

        const newElements = f.elements.map((e) => {
          if (e.id !== element.id) return e;

          // 如果没有映射项，删除 dictConfig；否则保存
          if (cleanedItems.length === 0) {
            const { dictConfig: _dictConfig, ...rest } = e;
            return rest;
          }

          return {
            ...e,
            dictConfig: {
              mapType: values.mapType,
              items: cleanedItems,
            },
          };
        });

        return { ...f, elements: newElements };
      });

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "SL651",
        config: { funcs: newFuncs },
      });

      onSuccess?.();
      setOpen(false);
    };

    const mapType = Form.useWatch("mapType", form);

    return (
      <Modal
        open={open}
        title={`字典配置 - ${element?.name}`}
        onCancel={() => setOpen(false)}
        onOk={handleOk}
        confirmLoading={saveMutation.isPending}
        forceRender
        width={800}
      >
        <Form form={form} layout="vertical">
          <Form.Item
            label="映射类型"
            name="mapType"
            rules={[{ required: true, message: "请选择映射类型" }]}
          >
            <Select
              options={[
                { value: "VALUE", label: "值映射 - 根据数值映射文本" },
                { value: "BIT", label: "位映射 - 根据二进制位映射文本" },
              ]}
              onChange={() => {
                // ✅ 切换映射类型时清空 items，避免数据混乱
                form.setFieldsValue({ items: [] });
              }}
            />
          </Form.Item>

          <Form.Item
            label="映射项"
            extra={
              mapType === "BIT"
                ? "配置二进制位对应的文本（位号范围 0-31，可选择位值为0或1时触发）"
                : "配置数值对应的文本"
            }
          >
            <Form.List name="items">
              {(fields, { add, remove }) => (
                <>
                  {fields.map(({ key, name: itemName, ...restField }) => (
                    <div
                      key={key}
                      style={{
                        marginBottom: 12,
                        border: "1px solid #f0f0f0",
                        padding: 12,
                        borderRadius: 4,
                      }}
                    >
                      {/* 主要字段 */}
                      {mapType === "BIT" ? (
                        <>
                          {/* 主要字段：位号、触发值、映射文本 */}
                          <Flex gap={8} align="center" wrap="nowrap">
                            <Form.Item
                              {...restField}
                              name={[itemName, "key"]}
                              rules={[
                                { required: true, message: "请输入位号" },
                                {
                                  pattern: /^([0-9]|[1-2][0-9]|3[0-1])$/,
                                  message: "位号范围 0-31",
                                },
                              ]}
                              style={{ flex: 1, marginBottom: 0 }}
                            >
                              <Input placeholder="位号(0-31)" />
                            </Form.Item>
                            <Form.Item
                              {...restField}
                              name={[itemName, "value"]}
                              initialValue="1"
                              rules={[{ required: true, message: "请选择触发值" }]}
                              style={{ width: 80, marginBottom: 0 }}
                            >
                              <Select
                                options={[
                                  { value: "1", label: "位=1" },
                                  { value: "0", label: "位=0" },
                                ]}
                              />
                            </Form.Item>
                            <Form.Item
                              {...restField}
                              name={[itemName, "label"]}
                              rules={[{ required: true, message: "" }]}
                              style={{ flex: 2, marginBottom: 0 }}
                            >
                              <Input placeholder="映射文本" />
                            </Form.Item>
                            <Tooltip title="添加依赖条件">
                              <Button
                                size="small"
                                type="text"
                                icon={<span style={{ fontSize: 16, fontWeight: "bold" }}>+</span>}
                                onClick={(e) => {
                                  e.stopPropagation();
                                  const conditions =
                                    form.getFieldValue([
                                      "items",
                                      itemName,
                                      "dependsOn",
                                      "conditions",
                                    ]) || [];
                                  form.setFieldValue(
                                    ["items", itemName, "dependsOn", "conditions"],
                                    [...conditions, { bitIndex: "", bitValue: "1" }]
                                  );
                                }}
                                style={{ flexShrink: 0 }}
                              />
                            </Tooltip>
                            <Button
                              type="text"
                              danger
                              onClick={(e) => {
                                e.stopPropagation();
                                remove(itemName);
                              }}
                              style={{ flexShrink: 0 }}
                            >
                              删除
                            </Button>
                          </Flex>

                          {/* ✅ operator 字段（在 Form.List 外面，避免路径冲突） */}
                          <Form.Item
                            noStyle
                            shouldUpdate={(prev, curr) => {
                              const prevConds = prev.items?.[itemName]?.dependsOn?.conditions;
                              const currConds = curr.items?.[itemName]?.dependsOn?.conditions;
                              return prevConds?.length !== currConds?.length;
                            }}
                          >
                            {() => {
                              const conditions =
                                form.getFieldValue([
                                  "items",
                                  itemName,
                                  "dependsOn",
                                  "conditions",
                                ]) || [];

                              return conditions.length > 0 ? (
                                <div
                                  style={{
                                    paddingLeft: 12,
                                    borderLeft: "2px solid #e8e8e8",
                                    marginTop: 8,
                                  }}
                                >
                                  <Flex
                                    justify="space-between"
                                    align="center"
                                    style={{ marginBottom: 8 }}
                                  >
                                    <span style={{ fontSize: 12, color: "#666" }}>依赖条件</span>
                                    <Form.Item
                                      name={[itemName, "dependsOn", "operator"]}
                                      initialValue="AND"
                                      style={{ marginBottom: 0 }}
                                    >
                                      <Select
                                        size="small"
                                        style={{ width: 140 }}
                                        options={[
                                          { value: "AND", label: "AND（全满足）" },
                                          { value: "OR", label: "OR（任一满足）" },
                                        ]}
                                      />
                                    </Form.Item>
                                  </Flex>
                                </div>
                              ) : null;
                            }}
                          </Form.Item>

                          {/* ✅ 依赖条件列表（嵌套的 Form.List） */}
                          <Form.List name={[itemName, "dependsOn", "conditions"]}>
                            {(condFields, { remove: removeCond }) => (
                              <>
                                {condFields.length > 0 && (
                                  <div
                                    style={{
                                      paddingLeft: 12,
                                      borderLeft: "2px solid #e8e8e8",
                                      marginTop: -8,
                                    }}
                                  >
                                    {condFields.map(
                                      ({ key: condKey, name: condName, ...condRestField }) => (
                                        <Flex
                                          key={condKey}
                                          gap={8}
                                          align="center"
                                          style={{ marginBottom: 4 }}
                                        >
                                          <span style={{ fontSize: 12, color: "#999", width: 60 }}>
                                            依赖位号
                                          </span>
                                          <Form.Item
                                            {...condRestField}
                                            name={[condName, "bitIndex"]}
                                            rules={[
                                              { required: true, message: "请输入位号" },
                                              {
                                                pattern: /^([0-9]|[1-2][0-9]|3[0-1])$/,
                                                message: "位号 0-31",
                                              },
                                            ]}
                                            style={{ flex: 1, marginBottom: 0 }}
                                          >
                                            <Input size="small" placeholder="位号(0-31)" />
                                          </Form.Item>
                                          <span style={{ fontSize: 12, color: "#999" }}>
                                            期望值
                                          </span>
                                          <Form.Item
                                            {...condRestField}
                                            name={[condName, "bitValue"]}
                                            initialValue="1"
                                            rules={[{ required: true, message: "请选择" }]}
                                            style={{ width: 80, marginBottom: 0 }}
                                          >
                                            <Select
                                              size="small"
                                              options={[
                                                { value: "1", label: "=1" },
                                                { value: "0", label: "=0" },
                                              ]}
                                            />
                                          </Form.Item>
                                          <Button
                                            size="small"
                                            type="text"
                                            danger
                                            onClick={(e) => {
                                              e.stopPropagation();
                                              removeCond(condName);
                                            }}
                                          >
                                            删除
                                          </Button>
                                        </Flex>
                                      )
                                    )}
                                  </div>
                                )}
                              </>
                            )}
                          </Form.List>
                        </>
                      ) : (
                        <Flex gap={8} align="center">
                          <Form.Item
                            {...restField}
                            name={[itemName, "key"]}
                            rules={[{ required: true, message: "请输入值" }]}
                            style={{ flex: 1, marginBottom: 0 }}
                          >
                            <Input placeholder="数值" />
                          </Form.Item>
                          <Form.Item
                            {...restField}
                            name={[itemName, "label"]}
                            rules={[{ required: true, message: "" }]}
                            style={{ flex: 1, marginBottom: 0 }}
                          >
                            <Input placeholder="映射文本" />
                          </Form.Item>
                          <Button type="text" danger onClick={() => remove(itemName)}>
                            删除
                          </Button>
                        </Flex>
                      )}
                    </div>
                  ))}
                  <Button
                    type="dashed"
                    onClick={() => {
                      // ✅ 根据映射类型提供正确的初始值
                      if (mapType === "BIT") {
                        add({
                          key: "",
                          label: "",
                          value: "1",
                          dependsOn: { operator: "AND", conditions: [] },
                        });
                      } else {
                        add({
                          key: "",
                          label: "",
                        });
                      }
                    }}
                    block
                  >
                    + 添加映射项
                  </Button>
                </>
              )}
            </Form.List>
          </Form.Item>
        </Form>
      </Modal>
    );
  }
);

export default SL651ConfigPage;
