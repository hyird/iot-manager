/**
 * 设备表单弹窗组件
 */

import { Form, Input, InputNumber, Modal, Select, Switch } from "antd";
import type { Device } from "@/types";

interface DeviceFormValues {
  id?: number;
  name: string;
  device_code: string;
  link_id: number;
  protocol_config_id: number;
  status: Device.Status;
  online_timeout?: number;
  remote_control?: boolean;
  modbus_mode?: Device.ModbusMode;
  timezone?: string;
  remark?: string;
}

interface LinkOption {
  id: number;
  name: string;
  mode: string;
  ip: string;
  port: number;
  protocol: string;
}

interface ProtocolOption {
  id: number;
  name: string;
}

interface DeviceFormModalProps {
  open: boolean;
  editing: Device.Item | null;
  loading: boolean;
  linkOptions: LinkOption[];
  protocolOptions: ProtocolOption[];
  onCancel: () => void;
  onFinish: (values: DeviceFormValues) => void;
}

const DeviceFormModal = ({
  open,
  editing,
  loading,
  linkOptions,
  protocolOptions,
  onCancel,
  onFinish,
}: DeviceFormModalProps) => {
  const [form] = Form.useForm<DeviceFormValues>();

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
        timezone: editing.timezone ?? "+08:00",
        remark: editing.remark,
      });
    } else if (isOpen) {
      form.resetFields();
      form.setFieldsValue({ status: "enabled", remote_control: true, timezone: "+08:00" });
    }
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
          onFinish(values);
          form.resetFields();
        }}
      >
        <Form.Item name="id" hidden>
          <Input />
        </Form.Item>
        <Form.Item
          label="设备名称"
          name="name"
          rules={[{ required: true, message: "请输入设备名称" }]}
        >
          <Input placeholder="设备名称" />
        </Form.Item>
        <Form.Item
          label="关联链路"
          name="link_id"
          rules={[{ required: true, message: "请选择关联链路" }]}
        >
          <Select placeholder="选择链路">
            {linkOptions.map((opt) => (
              <Select.Option key={opt.id} value={opt.id}>
                {opt.name} ({opt.mode} - {opt.ip}:{opt.port})
              </Select.Option>
            ))}
          </Select>
        </Form.Item>
        <Form.Item
          label="协议配置"
          name="protocol_config_id"
          rules={[{ required: true, message: "请选择协议配置" }]}
        >
          <Select placeholder="选择协议配置">
            {protocolOptions.map((opt) => (
              <Select.Option key={opt.id} value={opt.id}>
                {opt.name}
              </Select.Option>
            ))}
          </Select>
        </Form.Item>
        <Form.Item noStyle dependencies={["link_id"]}>
          {({ getFieldValue }) => {
            const linkId = getFieldValue("link_id");
            const selectedLink = linkOptions.find((opt) => opt.id === linkId);
            const showModbusMode =
              selectedLink?.mode === "TCP Server" && selectedLink?.protocol === "Modbus";
            if (!showModbusMode) return null;
            return (
              <Form.Item
                label="Modbus 模式"
                name="modbus_mode"
                rules={[{ required: true, message: "请选择 Modbus 模式" }]}
                extra="TCP Server 模式下需要指定设备的 Modbus 通信模式"
              >
                <Select placeholder="选择 Modbus 模式">
                  <Select.Option value="TCP">Modbus TCP</Select.Option>
                  <Select.Option value="RTU">Modbus RTU</Select.Option>
                </Select>
              </Form.Item>
            );
          }}
        </Form.Item>
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
        <Form.Item label="状态" name="status" rules={[{ required: true }]}>
          <Select>
            <Select.Option value="enabled">启用</Select.Option>
            <Select.Option value="disabled">禁用</Select.Option>
          </Select>
        </Form.Item>
        <Form.Item
          label="在线超时时间"
          name="online_timeout"
          extra="设备无心跳或数据上报超过此时间视为离线，单位：秒"
        >
          <InputNumber placeholder="默认 300 秒（5分钟）" min={1} className="!w-full" />
        </Form.Item>
        <Form.Item
          label="允许远控"
          name="remote_control"
          valuePropName="checked"
          extra="关闭后将禁止对该设备下发指令"
        >
          <Switch checkedChildren="是" unCheckedChildren="否" />
        </Form.Item>
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
        <Form.Item label="备注" name="remark">
          <Input.TextArea rows={3} placeholder="备注信息" />
        </Form.Item>
      </Form>
    </Modal>
  );
};

export default DeviceFormModal;

export type { DeviceFormValues };
