/**
 * Modbus 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
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
import type { Protocol, Modbus } from "@/types";
import { usePermission } from "@/hooks";
import { PageContainer } from "@/components/PageContainer";
import { useProtocolConfigList, useProtocolConfigSave, useProtocolConfigDelete } from "@/services";

const { Sider, Content } = Layout;

/** 寄存器类型选项 */
const RegisterTypeOptions: { value: Modbus.RegisterType; label: string }[] = [
  { value: "COIL", label: "线圈 (Coil)" },
  { value: "DISCRETE_INPUT", label: "离散输入 (Discrete Input)" },
  { value: "HOLDING_REGISTER", label: "保持寄存器 (Holding Register)" },
  { value: "INPUT_REGISTER", label: "输入寄存器 (Input Register)" },
];

/** 数据类型选项 */
const DataTypeOptions: { value: Modbus.DataType; label: string; quantity: number }[] = [
  { value: "BOOL", label: "BOOL (1 bit)", quantity: 1 },
  { value: "INT16", label: "INT16 (16位有符号)", quantity: 1 },
  { value: "UINT16", label: "UINT16 (16位无符号)", quantity: 1 },
  { value: "INT32", label: "INT32 (32位有符号)", quantity: 2 },
  { value: "UINT32", label: "UINT32 (32位无符号)", quantity: 2 },
  { value: "FLOAT32", label: "FLOAT32 (32位浮点)", quantity: 2 },
  { value: "INT64", label: "INT64 (64位有符号)", quantity: 4 },
  { value: "UINT64", label: "UINT64 (64位无符号)", quantity: 4 },
  { value: "DOUBLE", label: "DOUBLE (64位浮点)", quantity: 4 },
  { value: "STRING", label: "STRING (字符串)", quantity: 1 },
];

/** 大小端选项 */
const EndianOptions: { value: Modbus.Endian; label: string }[] = [
  { value: "BIG", label: "大端 (Big Endian)" },
  { value: "LITTLE", label: "小端 (Little Endian)" },
];

/** 字顺序选项 */
const WordOrderOptions: { value: Modbus.WordOrder; label: string }[] = [
  { value: "HIGH_FIRST", label: "高位在前 (AB CD)" },
  { value: "LOW_FIRST", label: "低位在前 (CD AB)" },
];

/** 生成唯一 ID */
const generateId = () => crypto.randomUUID();

/** 根据数据类型获取寄存器数量 */
const getQuantityByDataType = (dataType: Modbus.DataType): number => {
  const opt = DataTypeOptions.find((o) => o.value === dataType);
  return opt?.quantity ?? 1;
};

/** 检查寄存器地址是否冲突 */
const checkAddressConflict = (
  registers: Modbus.Register[],
  newRegister: { registerType: Modbus.RegisterType; address: number; quantity: number },
  excludeId?: string
): { conflict: boolean; conflictWith?: Modbus.Register } => {
  const newStart = newRegister.address;
  const newEnd = newRegister.address + newRegister.quantity - 1;

  for (const reg of registers) {
    // 跳过自身（编辑模式）
    if (excludeId && reg.id === excludeId) continue;
    // 只检查同类型寄存器
    if (reg.registerType !== newRegister.registerType) continue;

    const existStart = reg.address;
    const existEnd = reg.address + reg.quantity - 1;

    // 检查地址范围是否重叠
    if (!(newEnd < existStart || newStart > existEnd)) {
      return { conflict: true, conflictWith: reg };
    }
  }

  return { conflict: false };
};

/** 设备类型 Modal Ref */
interface DeviceTypeModalRef {
  open: (mode: "create" | "edit", data?: Protocol.Item) => void;
}

/** 寄存器 Modal Ref */
interface RegisterModalRef {
  open: (mode: "create" | "edit", typeId: number, register?: Modbus.Register) => void;
}

const ModbusConfigPage = () => {
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
  } = useProtocolConfigList({ protocol: "MODBUS" }, { enabled: canQuery });

  // 保存和删除 mutations
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();

  // 当前选中的设备类型 ID（用户手动选择）
  const [selectedTypeId, setSelectedTypeId] = useState<number>();

  // Modal refs
  const deviceTypeModalRef = useRef<DeviceTypeModalRef>(null);
  const registerModalRef = useRef<RegisterModalRef>(null);

  // 设备类型列表（使用 useMemo 保持引用稳定）
  const types = useMemo(() => configPage?.list || [], [configPage?.list]);

  // 计算当前激活的类型 ID：优先用户选择，否则默认第一个
  const activeTypeId = useMemo(() => {
    if (selectedTypeId && types.some((t) => t.id === selectedTypeId)) {
      return selectedTypeId;
    }
    return types.length > 0 ? types[0].id : undefined;
  }, [selectedTypeId, types]);

  // 当前激活的设备类型
  const activeType = useMemo(() => {
    return types.find((t) => t.id === activeTypeId);
  }, [activeTypeId, types]);

  // 寄存器列表（派生状态，根据 activeTypeId 计算）
  const registers = useMemo<Modbus.Register[]>(() => {
    if (!activeType) return [];
    const config = activeType.config as Modbus.Config;
    return config?.registers || [];
  }, [activeType]);

  // 加载状态（与数据加载状态同步）
  const loadingRegisters = loadingTypes;

  // ========== 设备类型操作 ==========

  const handleDeleteDeviceType = async () => {
    if (!activeTypeId) return;
    await deleteMutation.mutateAsync(activeTypeId);
    setSelectedTypeId(undefined);
  };

  // ========== 寄存器操作 ==========

  const handleDeleteRegister = async (registerId: string) => {
    if (!activeTypeId || !activeType) return;

    const config = activeType.config as Modbus.Config;
    const newConfig: Modbus.Config = {
      endian: config.endian,
      wordOrder: config.wordOrder,
      registers: config.registers.filter((r) => r.id !== registerId),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "MODBUS",
      config: newConfig,
    });
  };

  // ========== 寄存器表格列 ==========

  const registerColumns: ColumnsType<Modbus.Register> = [
    { title: "名称", dataIndex: "name", width: 120 },
    {
      title: "寄存器类型",
      dataIndex: "registerType",
      width: 200,
      minWidth: 150,
      render: (val: Modbus.RegisterType) => {
        const opt = RegisterTypeOptions.find((o) => o.value === val);
        const colorMap: Record<Modbus.RegisterType, string> = {
          COIL: "green",
          DISCRETE_INPUT: "blue",
          HOLDING_REGISTER: "orange",
          INPUT_REGISTER: "cyan",
        };
        return <Tag color={colorMap[val]}>{opt?.label || val}</Tag>;
      },
    },
    {
      title: "地址",
      dataIndex: "address",
      width: 80,
      render: (val: number) => String(val).padStart(4, "0"),
    },
    {
      title: "数据类型",
      dataIndex: "dataType",
      width: 160,
      render: (val: Modbus.DataType) => {
        const opt = DataTypeOptions.find((o) => o.value === val);
        return opt?.label || val;
      },
    },
    { title: "单位", dataIndex: "unit", width: 60 },
    { title: "备注", dataIndex: "remark", width: 150, ellipsis: true },
    {
      title: "操作",
      width: 120,
      render: (_, r) => (
        <Space>
          {canEdit && (
            <Button
              size="small"
              type="link"
              onClick={() => registerModalRef.current?.open("edit", activeTypeId!, r)}
            >
              编辑
            </Button>
          )}
          {canDelete && (
            <Popconfirm title="确认删除？" onConfirm={() => handleDeleteRegister(r.id)}>
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
      <PageContainer title="Modbus配置">
        <Result status="403" title="无权访问" subTitle="您没有权限访问此页面" />
      </PageContainer>
    );
  }

  return (
    <PageContainer title="Modbus配置">
      <Layout style={{ height: "100%", background: "transparent" }}>
        {/* 左侧：设备类型列表 */}
        <Sider width={320} style={{ background: "transparent", height: "100%", paddingRight: 12 }}>
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
                    onClick={() => deviceTypeModalRef.current?.open("edit", activeType)}
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
                  const config = t.config as Modbus.Config;
                  const regCount = config?.registers?.length || 0;
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
        </Sider>

        {/* 右侧：寄存器配置 */}
        <Content style={{ height: "100%" }}>
          <Card
            title={
              activeType ? (
                <Space>
                  <span>寄存器配置</span>
                  <Tag>
                    {(activeType.config as Modbus.Config)?.endian === "BIG" ? "大端" : "小端"}
                  </Tag>
                  <Tag>
                    {(activeType.config as Modbus.Config)?.wordOrder === "HIGH_FIRST"
                      ? "高位在前"
                      : "低位在前"}
                  </Tag>
                </Space>
              ) : (
                "请选择设备类型"
              )
            }
            style={{ height: "100%", display: "flex", flexDirection: "column" }}
            styles={{ body: { flex: 1, overflow: "auto", padding: 0 } }}
            extra={
              activeTypeId &&
              canAdd && (
                <Button
                  type="primary"
                  onClick={() => registerModalRef.current?.open("create", activeTypeId)}
                >
                  新增寄存器
                </Button>
              )
            }
          >
            {!activeTypeId ? (
              <Empty description="未选择设备类型" />
            ) : (
              <div style={{ "--ant-table-header-border-radius": 0 } as React.CSSProperties}>
                <Table
                  dataSource={registers}
                  rowKey="id"
                  pagination={false}
                  loading={loadingRegisters}
                  sticky
                  columns={registerColumns}
                  scroll={{ x: 1000 }}
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

      {/* 寄存器 Modal */}
      <RegisterModal
        ref={registerModalRef}
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
        if (data) {
          const config = data.config as Modbus.Config;
          form.setFieldsValue({
            name: data.name,
            enabled: data.enabled,
            endian: config?.endian || "BIG",
            wordOrder: config?.wordOrder || "HIGH_FIRST",
            remark: data.remark,
          });
        } else {
          form.setFieldsValue({
            enabled: true,
            endian: "BIG",
            wordOrder: "HIGH_FIRST",
          });
        }
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      const values = await form.validateFields();
      const existingConfig = (current?.config as Modbus.Config) || { registers: [] };

      await saveMutation.mutateAsync({
        id: current?.id,
        protocol: "MODBUS",
        name: values.name,
        enabled: values.enabled,
        config: {
          endian: values.endian,
          wordOrder: values.wordOrder,
          registers: existingConfig.registers || [],
        },
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
            <Input placeholder="如：温湿度传感器、电表" />
          </Form.Item>
          <Flex gap={16}>
            <Form.Item
              label="大小端"
              name="endian"
              rules={[{ required: true, message: "请选择大小端" }]}
              style={{ flex: 1 }}
            >
              <Select options={EndianOptions} />
            </Form.Item>
            <Form.Item
              label="字顺序"
              name="wordOrder"
              rules={[{ required: true, message: "请选择字顺序" }]}
              style={{ flex: 1 }}
              extra="多寄存器数据类型时使用"
            >
              <Select options={WordOrderOptions} />
            </Form.Item>
          </Flex>
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

// ========== 寄存器 Modal ==========

interface RegisterModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: ReturnType<typeof useProtocolConfigSave>;
}

const RegisterModal = forwardRef<RegisterModalRef, RegisterModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [mode, setMode] = useState<"create" | "edit">("create");
    const [typeId, setTypeId] = useState<number>();
    const [current, setCurrent] = useState<Modbus.Register>();
    const [form] = Form.useForm();

    // 监听寄存器类型变化
    const registerType = Form.useWatch("registerType", form);

    useImperativeHandle(ref, () => ({
      open(m, t, register) {
        setMode(m);
        setTypeId(t);
        setCurrent(register);
        form.resetFields();
        form.setFieldsValue(
          register ?? {
            registerType: "HOLDING_REGISTER",
            dataType: "UINT16",
          }
        );
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      if (!typeId) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      const config = type.config as Modbus.Config;
      const actualQuantity = getQuantityByDataType(values.dataType);

      // 检查地址冲突
      const conflictCheck = checkAddressConflict(
        config.registers || [],
        { registerType: values.registerType, address: values.address, quantity: actualQuantity },
        mode === "edit" ? current?.id : undefined
      );

      if (conflictCheck.conflict) {
        const reg = conflictCheck.conflictWith!;
        Modal.error({
          title: "地址冲突",
          content: `与寄存器「${reg.name}」地址范围冲突 (地址 ${reg.address}-${reg.address + reg.quantity - 1})`,
        });
        return;
      }

      let newRegisters: Modbus.Register[];

      if (mode === "create") {
        const newRegister: Modbus.Register = {
          id: generateId(),
          name: values.name,
          registerType: values.registerType,
          address: values.address,
          dataType: values.dataType,
          quantity: actualQuantity,
          unit: values.unit,
          remark: values.remark,
        };
        newRegisters = [...(config.registers || []), newRegister];
      } else {
        newRegisters = config.registers.map((r) =>
          r.id === current?.id
            ? {
                ...r,
                name: values.name,
                registerType: values.registerType,
                address: values.address,
                dataType: values.dataType,
                quantity: actualQuantity,
                unit: values.unit,
                remark: values.remark,
              }
            : r
        );
      }

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "MODBUS",
        config: {
          endian: config.endian,
          wordOrder: config.wordOrder,
          registers: newRegisters,
        },
      });

      onSuccess?.();
      setOpen(false);
    };

    // 判断是否为线圈或离散输入（只支持 BOOL）
    const isBitRegister = registerType === "COIL" || registerType === "DISCRETE_INPUT";

    return (
      <Modal
        title={mode === "create" ? "新增寄存器" : "编辑寄存器"}
        open={open}
        onOk={handleOk}
        onCancel={() => setOpen(false)}
        confirmLoading={saveMutation.isPending}
        forceRender
        width={600}
      >
        <Form form={form} layout="vertical">
          <Form.Item label="名称" name="name" rules={[{ required: true, message: "请输入名称" }]}>
            <Input placeholder="如：温度、湿度、电压" />
          </Form.Item>

          <Flex gap={16}>
            <Form.Item
              label="寄存器类型"
              name="registerType"
              rules={[{ required: true, message: "请选择寄存器类型" }]}
              style={{ flex: 1 }}
            >
              <Select
                options={RegisterTypeOptions}
                onChange={(val) => {
                  // 线圈和离散输入只支持 BOOL
                  if (val === "COIL" || val === "DISCRETE_INPUT") {
                    form.setFieldsValue({ dataType: "BOOL" });
                  }
                }}
              />
            </Form.Item>
            <Form.Item
              label="地址"
              name="address"
              rules={[{ required: true, message: "请输入地址" }]}
              style={{ flex: 1 }}
            >
              <InputNumber min={0} max={65535} style={{ width: "100%" }} placeholder="0-65535" />
            </Form.Item>
          </Flex>

          <Form.Item
            label="数据类型"
            name="dataType"
            rules={[{ required: true, message: "请选择数据类型" }]}
          >
            <Select
              options={
                isBitRegister
                  ? [{ value: "BOOL" as const, label: "BOOL (1 bit)", quantity: 1 }]
                  : DataTypeOptions
              }
              disabled={isBitRegister}
            />
          </Form.Item>

          <Flex gap={16}>
            <Form.Item label="单位" name="unit" style={{ flex: 1 }}>
              <Input placeholder="如：V、A、℃、%" />
            </Form.Item>
            <Form.Item label="备注" name="remark" style={{ flex: 1 }}>
              <Input placeholder="备注信息" />
            </Form.Item>
          </Flex>
        </Form>
      </Modal>
    );
  }
);

export default ModbusConfigPage;
