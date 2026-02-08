/**
 * SL651 应答要素 Modal
 */

import { Button, Form, Input, InputNumber, Modal, Select, Table, Tag } from "antd";
import { forwardRef, useImperativeHandle, useState } from "react";
import type { Protocol, SL651 } from "@/types";
import { EncodeList, generateId, type SaveMutation } from "./shared";

export interface ResponseElementsModalRef {
  open: (typeId: number, func: SL651.Func) => void;
}

interface ResponseElementsModalProps {
  types: Protocol.Item[];
  onSuccess?: () => void;
  saveMutation: SaveMutation;
}

const ResponseElementsModal = forwardRef<ResponseElementsModalRef, ResponseElementsModalProps>(
  ({ types, onSuccess, saveMutation }, ref) => {
    const [open, setOpen] = useState(false);
    const [typeId, setTypeId] = useState<number>();
    const [func, setFunc] = useState<SL651.Func>();
    const [form] = Form.useForm();

    useImperativeHandle(ref, () => ({
      open(t, f) {
        setTypeId(t);
        setFunc(f);
        form.resetFields();
        form.setFieldsValue({
          responseElements: f.responseElements || [],
        });
        setOpen(true);
      },
    }));

    const handleOk = async () => {
      if (!typeId || !func) return;
      const values = await form.validateFields();

      const type = types.find((t) => t.id === typeId);
      if (!type) return;

      // 过滤掉不完整的要素
      const cleanedElements = (values.responseElements || [])
        .filter(
          (ele: Partial<SL651.Element>) =>
            ele.name?.trim() && ele.guideHex?.trim() && ele.encode && ele.length !== undefined
        )
        .map((ele: Partial<SL651.Element>) => ({
          id: ele.id || generateId(),
          name: ele.name?.trim(),
          guideHex: ele.guideHex?.trim(),
          encode: ele.encode,
          length: ele.length,
          digits: Number.isFinite(ele.digits) ? ele.digits : 0,
          unit: ele.unit?.trim() || undefined,
          remark: ele.remark?.trim() || undefined,
        }));

      const config = type.config as SL651.Config;
      const newFuncs = config.funcs.map((f) => {
        if (f.id !== func.id) return f;
        return {
          ...f,
          responseElements: cleanedElements.length > 0 ? cleanedElements : undefined,
        };
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
        title={`应答要素配置 - ${func?.name} (${func?.funcCode})`}
        onCancel={() => setOpen(false)}
        onOk={handleOk}
        confirmLoading={saveMutation.isPending}
        forceRender
        width={900}
      >
        <div className="mb-4">
          <Tag color="blue">下行功能码</Tag>
          <span className="text-gray-500 ml-2">
            配置设备应答报文的解析要素。设备收到下行指令后会返回应答报文，这里定义如何解析应答报文中的数据。
          </span>
        </div>

        <Form form={form} layout="vertical">
          <Form.List name="responseElements">
            {(fields, { add, remove }) => (
              <>
                <Table
                  dataSource={fields}
                  rowKey="key"
                  pagination={false}
                  size="small"
                  scroll={{ x: "max-content" }}
                  columns={[
                    {
                      title: "要素名称",
                      width: 150,
                      render: (_, field) => (
                        <Form.Item
                          name={[field.name, "name"]}
                          rules={[{ required: true, message: "必填" }]}
                          className="!mb-0"
                        >
                          <Input placeholder="要素名称" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "引导符(HEX)",
                      width: 120,
                      render: (_, field) => (
                        <Form.Item
                          name={[field.name, "guideHex"]}
                          rules={[{ required: true, message: "必填" }]}
                          className="!mb-0"
                        >
                          <Input placeholder="如: 01" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "编码",
                      width: 140,
                      render: (_, field) => (
                        <Form.Item
                          name={[field.name, "encode"]}
                          rules={[{ required: true, message: "必填" }]}
                          className="!mb-0"
                        >
                          <Select
                            placeholder="选择"
                            options={EncodeList.map((e) => ({ value: e, label: e }))}
                          />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "长度",
                      width: 80,
                      render: (_, field) => (
                        <Form.Item
                          name={[field.name, "length"]}
                          rules={[{ required: true, message: "必填" }]}
                          className="!mb-0"
                        >
                          <InputNumber min={1} className="!w-full" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "小数位",
                      width: 80,
                      render: (_, field) => (
                        <Form.Item name={[field.name, "digits"]} className="!mb-0">
                          <InputNumber min={0} max={8} className="!w-full" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "单位",
                      width: 80,
                      render: (_, field) => (
                        <Form.Item name={[field.name, "unit"]} className="!mb-0">
                          <Input placeholder="单位" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "备注",
                      width: 120,
                      render: (_, field) => (
                        <Form.Item name={[field.name, "remark"]} className="!mb-0">
                          <Input placeholder="备注" />
                        </Form.Item>
                      ),
                    },
                    {
                      title: "操作",
                      width: 60,
                      fixed: "right" as const,
                      render: (_, field) => (
                        <Button type="text" danger onClick={() => remove(field.name)}>
                          删除
                        </Button>
                      ),
                    },
                  ]}
                  locale={{ emptyText: "暂无应答要素，点击下方按钮添加" }}
                />
                <Button
                  type="dashed"
                  onClick={() =>
                    add({
                      id: generateId(),
                      name: "",
                      guideHex: "",
                      encode: "BCD",
                      length: 1,
                      digits: 0,
                    })
                  }
                  block
                  className="mt-3"
                >
                  + 添加应答要素
                </Button>
              </>
            )}
          </Form.List>
        </Form>
      </Modal>
    );
  }
);

export default ResponseElementsModal;
