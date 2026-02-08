/**
 * Modbus 协议配置页面
 * 布局：左侧设备类型列表 + 右侧寄存器配置
 */

import { DownloadOutlined, UploadOutlined } from "@ant-design/icons";
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
  Tree,
} from "antd";
import type { ColumnsType } from "antd/es/table";
import { forwardRef, useImperativeHandle, useMemo, useRef, useState } from "react";
import { PageContainer } from "@/components/PageContainer";
import { usePermission, useProtocolImportExport } from "@/hooks";
import { useProtocolConfigDelete, useProtocolConfigList, useProtocolConfigSave } from "@/services";
import type { Modbus, ModbusDictConfig, Protocol } from "@/types";

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
];

/** 字节序选项 */
const ByteOrderOptions: { value: Modbus.ByteOrder; label: string }[] = [
  { value: "BIG_ENDIAN", label: "Big-endian" },
  { value: "LITTLE_ENDIAN", label: "Little-endian" },
  { value: "BIG_ENDIAN_BYTE_SWAP", label: "Big-endian byte swap" },
  { value: "LITTLE_ENDIAN_BYTE_SWAP", label: "Little-endian byte swap" },
];

/** 生成唯一 ID（兼容非安全上下文） */
const generateId = (): string =>
  "10000000-1000-4000-8000-100000000000".replace(/[018]/g, (c) =>
    (+c ^ (crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
  );

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
  const canImport = usePermission("iot:protocol:import");
  const canExport = usePermission("iot:protocol:export");

  // 设备类型列表查询
  const {
    data: configPage,
    isLoading: loadingTypes,
    refetch: refetchTypes,
  } = useProtocolConfigList({ protocol: "Modbus" }, { enabled: canQuery });

  // 保存和删除 mutations
  const saveMutation = useProtocolConfigSave();
  const deleteMutation = useProtocolConfigDelete();

  // 导入导出
  const { exportConfigs, triggerImport, importing } = useProtocolImportExport("Modbus");

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
    // 删除前记录下一个可选中的类型
    const idx = types.findIndex((t) => t.id === activeTypeId);
    const nextType = types[idx + 1] ?? types[idx - 1];
    await deleteMutation.mutateAsync(activeTypeId);
    setSelectedTypeId(nextType?.id);
  };

  // ========== 寄存器操作 ==========

  const handleDeleteRegister = async (registerId: string) => {
    if (!activeTypeId || !activeType) return;

    const config = activeType.config as Modbus.Config;
    const newConfig: Modbus.Config = {
      byteOrder: config.byteOrder,
      readInterval: config.readInterval,
      registers: config.registers.filter((r) => r.id !== registerId),
    };

    await saveMutation.mutateAsync({
      id: activeTypeId,
      protocol: "Modbus",
      config: newConfig,
    });
  };

  // ========== 寄存器表格列 ==========

  const registerColumns: ColumnsType<Modbus.Register> = [
    { title: "名称", dataIndex: "name" },
    {
      title: "寄存器类型",
      dataIndex: "registerType",
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
      render: (val: number, r: Modbus.Register) => {
        const prefixMap: Record<Modbus.RegisterType, string> = {
          COIL: "0",
          DISCRETE_INPUT: "1",
          INPUT_REGISTER: "3",
          HOLDING_REGISTER: "4",
        };
        return prefixMap[r.registerType] + String(val).padStart(4, "0");
      },
    },
    {
      title: "数据类型",
      dataIndex: "dataType",
      render: (val: Modbus.DataType) => {
        const opt = DataTypeOptions.find((o) => o.value === val);
        return opt?.label || val;
      },
    },
    { title: "单位", dataIndex: "unit" },
    { title: "备注", dataIndex: "remark", ellipsis: true },
    {
      title: "操作",
      width: 120,
      fixed: "end" as const,
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
      <div className="h-full flex">
        {/* 左侧：设备类型列表 */}
        <div className="w-[360px] shrink-0 pr-3 h-full">
          <Card
            title="设备类型"
            className="h-full flex flex-col"
            styles={{ body: { flex: 1, overflow: "auto", padding: 16 } }}
            extra={
              <Space size={4}>
                {canAdd && (
                  <Button
                    size="small"
                    type="primary"
                    onClick={() => deviceTypeModalRef.current?.open("create")}
                  >
                    新增
                  </Button>
                )}
                {canEdit && (
                  <Button
                    size="small"
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
                    <Button size="small" danger disabled={!activeTypeId}>
                      删除
                    </Button>
                  </Popconfirm>
                )}
                {canExport && (
                  <Tooltip title="导出">
                    <Button
                      size="small"
                      icon={<DownloadOutlined />}
                      disabled={!types.length}
                      onClick={() => exportConfigs(types)}
                    />
                  </Tooltip>
                )}
                {canImport && (
                  <Tooltip title="导入">
                    <Button
                      size="small"
                      icon={<UploadOutlined />}
                      loading={importing}
                      onClick={triggerImport}
                    />
                  </Tooltip>
                )}
              </Space>
            }
          >
            {loadingTypes ? (
              <Skeleton active paragraph={{ rows: 6 }} />
            ) : types.length === 0 ? (
              <Empty description="暂无设备类型" />
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
                        <Flex justify="space-between" align="center" className="h-8 p-1">
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
        </div>

        {/* 右侧：寄存器配置 */}
        <div className="flex-1 min-w-0 h-full">
          <Card
            title={
              activeType ? (
                <Space>
                  <span>寄存器配置</span>
                  <Tag>
                    {ByteOrderOptions.find(
                      (o) => o.value === (activeType.config as Modbus.Config)?.byteOrder
                    )?.label || "Big-endian"}
                  </Tag>
                  <Tag>间隔 {(activeType.config as Modbus.Config)?.readInterval ?? 1}s</Tag>
                </Space>
              ) : (
                "请选择设备类型"
              )
            }
            className="h-full flex flex-col"
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
                  scroll={{ x: "max-content" }}
                />
              </div>
            )}
          </Card>
        </div>
      </div>

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
            byteOrder: config?.byteOrder || "BIG_ENDIAN",
            readInterval: Number(config?.readInterval) || 1,
            remark: data.remark,
          });
        } else {
          form.setFieldsValue({
            enabled: true,
            byteOrder: "BIG_ENDIAN",
            readInterval: 1,
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
        protocol: "Modbus",
        name: values.name,
        enabled: values.enabled,
        config: {
          byteOrder: values.byteOrder,
          readInterval: values.readInterval,
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
          <Form.Item
            label="字节序"
            name="byteOrder"
            rules={[{ required: true, message: "请选择字节序" }]}
          >
            <Select options={ByteOrderOptions} />
          </Form.Item>
          <Form.Item label="读取间隔" name="readInterval" extra="轮询读取寄存器的时间间隔">
            <InputNumber min={1} max={3600} className="!w-full" addonAfter="秒" />
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

    // 监听寄存器类型和数据类型变化
    const registerType = Form.useWatch("registerType", form);
    const dataType = Form.useWatch("dataType", form);

    useImperativeHandle(ref, () => ({
      open(m, t, register) {
        setMode(m);
        setTypeId(t);
        setCurrent(register);
        form.resetFields();
        if (register) {
          form.setFieldsValue({
            ...register,
            boolLabel0: register.dictConfig?.items?.find((i) => i.key === "0")?.label,
            boolLabel1: register.dictConfig?.items?.find((i) => i.key === "1")?.label,
          });
        } else {
          form.setFieldsValue({
            registerType: "HOLDING_REGISTER",
            dataType: "INT16",
          });
        }
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

      // 检查地址 + 数量是否溢出 uint16 范围
      if (values.address + actualQuantity - 1 > 65535) {
        Modal.error({
          title: "地址溢出",
          content: `地址 ${values.address} + 数量 ${actualQuantity} 超出范围（末地址不能超过 65535）`,
        });
        return;
      }

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

      // 构建 dictConfig（Bool 值映射；非 BOOL 类型保留已有配置）
      let dictConfig: ModbusDictConfig | undefined;
      if (values.dataType === "BOOL") {
        dictConfig =
          values.boolLabel0 || values.boolLabel1
            ? {
                items: [
                  ...(values.boolLabel0 ? [{ key: "0", label: values.boolLabel0 }] : []),
                  ...(values.boolLabel1 ? [{ key: "1", label: values.boolLabel1 }] : []),
                ],
              }
            : undefined;
      } else {
        // 非 BOOL 类型：编辑时保留已有 dictConfig，创建时无
        dictConfig = mode === "edit" ? current?.dictConfig : undefined;
      }

      const registerFields = {
        name: values.name,
        registerType: values.registerType,
        address: values.address,
        dataType: values.dataType,
        quantity: actualQuantity,
        unit: values.unit,
        decimals: values.decimals,
        dictConfig,
        remark: values.remark,
      };

      let newRegisters: Modbus.Register[];

      if (mode === "create") {
        newRegisters = [...(config.registers || []), { id: generateId(), ...registerFields }];
      } else {
        newRegisters = config.registers.map((r) =>
          r.id === current?.id ? { ...r, ...registerFields } : r
        );
      }

      await saveMutation.mutateAsync({
        id: typeId,
        protocol: "Modbus",
        config: {
          byteOrder: config.byteOrder,
          readInterval: config.readInterval,
          registers: newRegisters,
        },
      });

      onSuccess?.();
      setOpen(false);
    };

    // 线圈/离散输入只支持 BOOL；保持寄存器/输入寄存器不支持 BOOL
    const isBitRegister = registerType === "COIL" || registerType === "DISCRETE_INPUT";
    const isWordRegister = registerType === "HOLDING_REGISTER" || registerType === "INPUT_REGISTER";

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
              className="flex-1"
            >
              <Select
                options={RegisterTypeOptions}
                onChange={(val) => {
                  if (val === "COIL" || val === "DISCRETE_INPUT") {
                    form.setFieldsValue({
                      dataType: "BOOL",
                      decimals: undefined,
                    });
                  } else if (form.getFieldValue("dataType") === "BOOL") {
                    form.setFieldsValue({
                      dataType: "INT16",
                      decimals: undefined,
                      boolLabel0: undefined,
                      boolLabel1: undefined,
                    });
                  }
                }}
              />
            </Form.Item>
            <Form.Item
              label="地址"
              name="address"
              rules={[{ required: true, message: "请输入地址" }]}
              className="flex-1"
            >
              <InputNumber min={0} max={65535} className="!w-full" placeholder="0-65535" />
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
                  : isWordRegister
                    ? DataTypeOptions.filter((o) => o.value !== "BOOL")
                    : DataTypeOptions
              }
              disabled={isBitRegister}
              onChange={() => {
                form.setFieldsValue({
                  decimals: undefined,
                  boolLabel0: undefined,
                  boolLabel1: undefined,
                });
              }}
            />
          </Form.Item>

          {(dataType === "FLOAT32" || dataType === "DOUBLE") && (
            <Form.Item label="小数位数" name="decimals">
              <InputNumber min={0} max={8} placeholder="不限制" className="!w-full" />
            </Form.Item>
          )}

          {dataType === "BOOL" && (
            <Flex gap={16}>
              <Form.Item label="0 值显示" name="boolLabel0" className="flex-1">
                <Input placeholder="如：关闭、OFF" />
              </Form.Item>
              <Form.Item label="1 值显示" name="boolLabel1" className="flex-1">
                <Input placeholder="如：开启、ON" />
              </Form.Item>
            </Flex>
          )}

          <Flex gap={16}>
            <Form.Item label="单位" name="unit" className="flex-1">
              <Input placeholder="如：V、A、℃、%" />
            </Form.Item>
            <Form.Item label="备注" name="remark" className="flex-1">
              <Input placeholder="备注信息" />
            </Form.Item>
          </Flex>
        </Form>
      </Modal>
    );
  }
);

export default ModbusConfigPage;
