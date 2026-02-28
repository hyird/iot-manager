/**
 * 设备表单弹窗组件
 */

import { Collapse, Form, Input, InputNumber, Modal, Select, Switch, TreeSelect } from "antd";
import type { RuleObject } from "antd/es/form";
import { useMemo } from "react";

import { useDeviceGroupTree } from "@/services";
import { useProtocolConfigOptions } from "@/services/protocol";
import type { Device, DeviceGroup, Link, Protocol } from "@/types";

/** HEX 内容校验规则 */
const hexContentRules: RuleObject[] = [
  { required: true, message: "请输入十六进制内容" },
  { pattern: /^[0-9A-Fa-f\s]+$/, message: "只能包含十六进制字符 (0-9, A-F)" },
  {
    validator: (_, value: string) => {
      if (!value) return Promise.resolve();
      const stripped = value.replace(/\s/g, "");
      if (stripped.length === 0) return Promise.reject("内容不能为空");
      if (stripped.length % 2 !== 0) return Promise.reject("十六进制长度必须为偶数（完整字节）");
      return Promise.resolve();
    },
  },
];

interface DeviceFormValues {
  id?: number;
  name: string;
  device_code?: string;
  link_id: number;
  protocol_config_id: number;
  status: Device.Status;
  online_timeout?: number;
  remote_control?: boolean;
  modbus_mode?: Device.ModbusMode;
  slave_id?: number;
  timezone?: string;
  heartbeat?: Device.HeartbeatConfig;
  registration?: Device.RegistrationConfig;
  group_id?: number | null;
  remark?: string;
}

interface DeviceFormModalProps {
  open: boolean;
  editing: Device.Item | null;
  loading: boolean;
  linkOptions: Link.Option[];
  onCancel: () => void;
  onFinish: (values: DeviceFormValues) => void;
}

/** 链路协议 → 协议配置类型 映射 */
const toProtocolType = (linkProtocol?: string): Protocol.Type | undefined => {
  if (!linkProtocol) return undefined;
  if (linkProtocol === "SL651") return "SL651";
  return "Modbus";
};

const DeviceFormModal = ({
  open,
  editing,
  loading,
  linkOptions,
  onCancel,
  onFinish,
}: DeviceFormModalProps) => {
  const [form] = Form.useForm<DeviceFormValues>();
  const { data: groupTree = [] } = useDeviceGroupTree();
  const linkId = Form.useWatch("link_id", form);

  const selectedLink = linkOptions.find((opt) => opt.id === linkId);
  const protocolType = toProtocolType(selectedLink?.protocol);
  // 心跳包/注册包仅 TCP Server 模式需要（设备主动连入，需要识别身份）
  // TCP Client 模式主动连接设备，无需额外识别
  const isModbusRtu = protocolType === "Modbus" && selectedLink?.mode === "TCP Server";

  const { data: protocolOptions } = useProtocolConfigOptions(protocolType!, {
    enabled: !!protocolType,
  });

  type GroupSelectNode = { value: number; title: string; children?: GroupSelectNode[] };

  const groupTreeSelectData = useMemo(() => {
    const convert = (nodes: DeviceGroup.TreeItem[]): GroupSelectNode[] =>
      nodes.map((n) => ({
        value: n.id,
        title: n.name,
        children: n.children?.length ? convert(n.children) : undefined,
      }));
    return convert(groupTree);
  }, [groupTree]);

  const handleOpen = (isOpen: boolean) => {
    if (isOpen && editing) {
      form.setFieldsValue({
        id: editing.id,
        name: editing.name,
        device_code: editing.device_code,
        link_id: editing.link_id,
        protocol_config_id: editing.protocol_config_id,
        status: editing.status,
        online_timeout: editing.online_timeout,
        remote_control: editing.remote_control ?? true,
        modbus_mode: editing.modbus_mode,
        slave_id: editing.slave_id ?? 1,
        timezone: editing.timezone ?? "+08:00",
        heartbeat: editing.heartbeat ?? { mode: "OFF" },
        registration: editing.registration ?? { mode: "OFF" },
        group_id: editing.group_id || undefined,
        remark: editing.remark,
      });
    } else if (isOpen) {
      form.resetFields();
      form.setFieldsValue({
        status: "enabled",
        remote_control: true,
        timezone: "+08:00",
        heartbeat: { mode: "OFF" },
        registration: { mode: "OFF" },
      });
    }
  };

  /** 链路变更时清除设备类型选择 */
  const handleLinkChange = () => {
    form.setFieldValue("protocol_config_id", undefined);
  };

  return (
    <Modal
      open={open}
      title={editing ? "编辑设备" : "新建设备"}
      onCancel={() => {
        onCancel();
        form.resetFields();
      }}
      onOk={() => form.submit()}
      confirmLoading={loading}
      destroyOnHidden
      width={520}
      afterOpenChange={handleOpen}
    >
      <Form<DeviceFormValues>
        form={form}
        layout="vertical"
        onFinish={(values) => {
          // HEX 内容去除空格
          if (values.heartbeat?.mode === "HEX" && values.heartbeat?.content) {
            values.heartbeat.content = values.heartbeat.content.replace(/\s/g, "");
          }
          if (values.registration?.mode === "HEX" && values.registration?.content) {
            values.registration.content = values.registration.content.replace(/\s/g, "");
          }
          onFinish(values);
          form.resetFields();
        }}
      >
        <Form.Item name="id" hidden>
          <Input />
        </Form.Item>

        {/* 基本信息 */}
        <Form.Item
          label="设备名称"
          name="name"
          rules={[{ required: true, message: "请输入设备名称" }]}
        >
          <Input placeholder="设备名称" />
        </Form.Item>
        <Form.Item label="所属分组" name="group_id">
          <TreeSelect
            allowClear
            treeData={groupTreeSelectData}
            placeholder="不选则不归属任何分组"
            treeDefaultExpandAll
            fieldNames={{ label: "title", value: "value" }}
          />
        </Form.Item>
        <Form.Item
          label="关联链路"
          name="link_id"
          rules={[{ required: true, message: "请选择关联链路" }]}
        >
          <Select placeholder="选择链路" onChange={handleLinkChange} disabled={!!editing}>
            {linkOptions.map((opt) => (
              <Select.Option key={opt.id} value={opt.id}>
                {opt.name} ({opt.protocol} - {opt.mode} - {opt.ip}:{opt.port})
              </Select.Option>
            ))}
          </Select>
        </Form.Item>
        <Form.Item
          label="设备类型"
          name="protocol_config_id"
          rules={[{ required: true, message: "请选择设备类型" }]}
        >
          <Select
            placeholder={linkId ? "选择设备类型" : "请先选择链路"}
            disabled={!!editing || !linkId}
          >
            {(protocolOptions?.list || []).map((opt) => (
              <Select.Option key={opt.id} value={opt.id}>
                {opt.name}
              </Select.Option>
            ))}
          </Select>
        </Form.Item>
        <Form.Item noStyle dependencies={["link_id"]}>
          {({ getFieldValue }) => {
            const currentLinkId = getFieldValue("link_id");
            const currentLink = linkOptions.find((opt) => opt.id === currentLinkId);
            const showModbusMode = currentLink?.protocol === "Modbus";
            if (!showModbusMode) return null;
            return (
              <Form.Item
                label="Modbus 模式"
                name="modbus_mode"
                rules={[{ required: true, message: "请选择 Modbus 模式" }]}
                extra={
                  currentLink?.mode === "TCP Server"
                    ? "TCP Server：选 RTU 表示 DTU 串口透传，选 TCP 表示设备直接以 ModbusTCP 连入"
                    : "TCP Client：需指定 Modbus 通信模式"
                }
              >
                <Select placeholder="选择 Modbus 模式">
                  <Select.Option value="TCP">Modbus TCP</Select.Option>
                  <Select.Option value="RTU">Modbus RTU</Select.Option>
                </Select>
              </Form.Item>
            );
          }}
        </Form.Item>
        {protocolType === "Modbus" && (
          <Form.Item
            label="从站地址 (Slave ID)"
            name="slave_id"
            rules={[{ required: true, message: "请输入从站地址" }]}
            extra="Modbus 从站地址，范围 1-247"
          >
            <InputNumber min={1} max={247} placeholder="默认 1" className="!w-full" />
          </Form.Item>
        )}
        {protocolType === "SL651" && (
          <Form.Item
            label="设备编码"
            name="device_code"
            rules={[
              { required: true, message: "请输入设备编码" },
              { pattern: /^[A-Za-z0-9]+$/, message: "设备编码只能包含字母和数字" },
            ]}
            extra="遥测站地址，用于协议通信识别"
          >
            <Input placeholder="如: 12345678" />
          </Form.Item>
        )}
        <Form.Item label="状态" name="status" rules={[{ required: true }]}>
          <Select>
            <Select.Option value="enabled">启用</Select.Option>
            <Select.Option value="disabled">禁用</Select.Option>
          </Select>
        </Form.Item>

        {/* 高级配置 */}
        <Collapse
          ghost
          className="!-mx-6 !mb-0"
          items={[
            {
              key: "advanced",
              label: "高级配置",
              children: (
                <>
                  {protocolType === "SL651" && (
                    <Form.Item
                      label="在线超时时间"
                      name="online_timeout"
                      extra="设备无心跳或数据上报超过此时间视为离线，单位：秒"
                    >
                      <InputNumber placeholder="默认 300 秒（5分钟）" min={1} className="!w-full" />
                    </Form.Item>
                  )}
                  {protocolType === "SL651" && (
                    <Form.Item
                      label="允许远控"
                      name="remote_control"
                      valuePropName="checked"
                      extra="关闭后将禁止对该设备下发指令"
                    >
                      <Switch checkedChildren="是" unCheckedChildren="否" />
                    </Form.Item>
                  )}
                  {protocolType === "SL651" && (
                    <Form.Item
                      label="设备时区"
                      name="timezone"
                      extra="设备上报时间所属时区，用于正确存储时间戳"
                    >
                      <Select>
                        <Select.Option value="+08:00">UTC+8（中国标准时间）</Select.Option>
                        <Select.Option value="+09:00">UTC+9（日本/韩国）</Select.Option>
                        <Select.Option value="+07:00">UTC+7（东南亚）</Select.Option>
                        <Select.Option value="+05:30">UTC+5:30（印度）</Select.Option>
                        <Select.Option value="+00:00">UTC+0（格林威治）</Select.Option>
                      </Select>
                    </Form.Item>
                  )}
                  {isModbusRtu && (
                    <>
                      <Form.Item label="心跳包模式" name={["heartbeat", "mode"]}>
                        <Select>
                          <Select.Option value="OFF">关闭</Select.Option>
                          <Select.Option value="HEX">HEX</Select.Option>
                          <Select.Option value="ASCII">ASCII</Select.Option>
                        </Select>
                      </Form.Item>
                      <Form.Item noStyle dependencies={[["heartbeat", "mode"]]}>
                        {({ getFieldValue }) => {
                          const hbMode = getFieldValue(["heartbeat", "mode"]);
                          if (hbMode === "OFF" || !hbMode) return null;
                          return (
                            <Form.Item
                              label="心跳包内容"
                              name={["heartbeat", "content"]}
                              rules={
                                hbMode === "HEX"
                                  ? hexContentRules
                                  : [{ required: true, message: "请输入心跳包内容" }]
                              }
                              extra={
                                hbMode === "HEX"
                                  ? "十六进制字符串，如: AA BB CC DD（空格会自动去除）"
                                  : "ASCII 字符串，支持 \\r \\n 转义"
                              }
                            >
                              <Input
                                placeholder={hbMode === "HEX" ? "AA BB CC DD" : "HELLO\\r\\n"}
                              />
                            </Form.Item>
                          );
                        }}
                      </Form.Item>
                      <Form.Item label="注册包模式" name={["registration", "mode"]}>
                        <Select>
                          <Select.Option value="OFF">关闭</Select.Option>
                          <Select.Option value="HEX">HEX</Select.Option>
                          <Select.Option value="ASCII">ASCII</Select.Option>
                        </Select>
                      </Form.Item>
                      <Form.Item noStyle dependencies={[["registration", "mode"]]}>
                        {({ getFieldValue }) => {
                          const regMode = getFieldValue(["registration", "mode"]);
                          if (regMode === "OFF" || !regMode) return null;
                          return (
                            <Form.Item
                              label="注册包内容"
                              name={["registration", "content"]}
                              rules={
                                regMode === "HEX"
                                  ? hexContentRules
                                  : [{ required: true, message: "请输入注册包内容" }]
                              }
                              extra={
                                regMode === "HEX"
                                  ? "十六进制字符串，如: AA BB CC DD（空格会自动去除）"
                                  : "ASCII 字符串，支持 \\r \\n 转义"
                              }
                            >
                              <Input
                                placeholder={regMode === "HEX" ? "AA BB CC DD" : "HELLO\\r\\n"}
                              />
                            </Form.Item>
                          );
                        }}
                      </Form.Item>
                    </>
                  )}
                  <Form.Item label="备注" name="remark">
                    <Input.TextArea rows={3} placeholder="备注信息" />
                  </Form.Item>
                </>
              ),
            },
          ]}
        />
      </Form>
    </Modal>
  );
};

export default DeviceFormModal;

export type { DeviceFormValues };
