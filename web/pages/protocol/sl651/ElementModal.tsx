/**
 * SL651 要素 Modal
 */

import { Form, Input, InputNumber, Modal, Select } from "antd";
import { forwardRef, useImperativeHandle, useState } from "react";
import type { Protocol, SL651 } from "@/types";
import { EncodeList, generateId, type SaveMutation } from "./shared";

export interface ElementModalRef {
  open: (mode: "create" | "edit", typeId: number, funcId: string, element?: SL651.Element) => void;
}

interface ElementModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: SaveMutation;
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
        form.setFieldsValue(element ?? { encode: "BCD", length: 1, digits: 0 });
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
            <InputNumber min={1} className="!w-full" />
          </Form.Item>
          <Form.Item
            label="小数位"
            name="digits"
            rules={[{ required: true, message: "请输入小数位" }]}
          >
            <InputNumber min={0} max={8} className="!w-full" />
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

export default ElementModal;
