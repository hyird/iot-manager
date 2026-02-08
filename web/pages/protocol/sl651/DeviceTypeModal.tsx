/**
 * SL651 设备类型 Modal
 */

import { Form, Input, Modal, Select, Switch } from "antd";
import { forwardRef, useImperativeHandle, useState } from "react";
import type { Protocol, SL651 } from "@/types";
import type { SaveMutation } from "./shared";

export interface DeviceTypeModalRef {
  open: (mode: "create" | "edit", data?: Protocol.Item) => void;
}

interface DeviceTypeModalProps {
  onSuccess?: () => void;
  saveMutation: SaveMutation;
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

export default DeviceTypeModal;
