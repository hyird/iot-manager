import { PlusOutlined } from "@ant-design/icons";
import { type UseMutationResult, useQueryClient } from "@tanstack/react-query";
import { Button, Form, Input, InputNumber, Modal, Select, Spin } from "antd";
import { useEffect, useMemo, useState } from "react";
import { useDeviceDetail, useDeviceOptions, useProtocolConfigDetail } from "@/services";
import type { Alert, Modbus, SL651 } from "@/types";
import { ConditionEditor } from "./ConditionEditor";

interface AlertRuleFormValues {
  id?: number;
  name: string;
  device_id: number;
  severity: Alert.Severity;
  conditions: Alert.Condition[];
  logic: "and" | "or";
  silence_duration: number;
  status: "enabled" | "disabled";
  remark?: string;
}

interface AlertRuleFormModalProps {
  open: boolean;
  editing: Alert.RuleItem | null;
  saveMutation: UseMutationResult<void, Error, Alert.RuleDto & { id?: number }, unknown>;
  onClose: () => void;
}

export function AlertRuleFormModal({
  open,
  editing,
  saveMutation,
  onClose,
}: AlertRuleFormModalProps) {
  const [form] = Form.useForm<AlertRuleFormValues>();
  const [conditions, setConditions] = useState<Alert.Condition[]>([]);
  const queryClient = useQueryClient();

  const { data: deviceOptions = [] } = useDeviceOptions({ enabled: open });

  // 监听选中的设备 ID
  const selectedDeviceId = Form.useWatch("device_id", form);

  // 链式查询：设备 → 协议配置 → 要素列表
  const { data: deviceDetail } = useDeviceDetail(selectedDeviceId ?? 0, {
    enabled: open && !!selectedDeviceId,
  });

  const protocolConfigId = deviceDetail?.protocol_config_id ?? 0;
  const protocolType = deviceDetail?.protocol_type;

  const { data: protocolConfig, isLoading: isLoadingConfig } = useProtocolConfigDetail(
    protocolConfigId,
    { enabled: open && protocolConfigId > 0 }
  );

  // 从协议配置中提取要素选项
  const elementOptions = useMemo(() => {
    if (!protocolConfig?.config) return [];

    const type = protocolType || protocolConfig.protocol;

    if (type === "SL651") {
      const config = protocolConfig.config as SL651.Config;
      // data key 格式与后端 SL651 解析器一致: funcCode_guideHex
      return (config.funcs ?? []).flatMap((f) =>
        (f.elements ?? []).map((e) => {
          const dataKey = `${f.funcCode}_${e.guideHex}`;
          return { value: dataKey, label: e.name };
        })
      );
    }

    if (type === "Modbus") {
      const config = protocolConfig.config as Modbus.Config;
      // data key 格式与后端 Modbus 解析器一致: registerType_address
      return (config.registers ?? []).map((r) => {
        const dataKey = `${r.registerType}_${r.address}`;
        return { value: dataKey, label: r.name };
      });
    }

    return [];
  }, [protocolConfig, protocolType]);

  useEffect(() => {
    if (open) {
      if (editing) {
        form.setFieldsValue({
          id: editing.id,
          name: editing.name,
          device_id: editing.device_id,
          severity: editing.severity,
          logic: editing.logic,
          silence_duration: editing.silence_duration,
          status: editing.status,
          remark: editing.remark,
        });
        setConditions(editing.conditions || []);
      } else {
        form.resetFields();
        form.setFieldsValue({
          severity: "warning",
          logic: "and",
          silence_duration: 300,
          status: "enabled",
        });
        setConditions([]);
      }
    }
  }, [open, editing, form]);

  const addCondition = () => {
    setConditions((prev) => [
      ...prev,
      { type: "threshold", elementKey: "", operator: ">", value: "" },
    ]);
  };

  const updateCondition = (index: number, value: Alert.Condition) => {
    setConditions((prev) => prev.map((c, i) => (i === index ? value : c)));
  };

  const removeCondition = (index: number) => {
    setConditions((prev) => prev.filter((_, i) => i !== index));
  };

  const onFinish = (values: AlertRuleFormValues) => {
    saveMutation.mutate(
      { ...values, conditions },
      {
        onSuccess: () => {
          onClose();
          queryClient.invalidateQueries({ queryKey: ["alert"] });
        },
      }
    );
  };

  return (
    <Modal
      open={open}
      title={editing ? "编辑规则" : "新建规则"}
      onCancel={onClose}
      onOk={() => form.submit()}
      confirmLoading={saveMutation.isPending}
      destroyOnHidden
      width={640}
    >
      <Form<AlertRuleFormValues> form={form} layout="vertical" onFinish={onFinish}>
        <Form.Item name="id" hidden>
          <Input />
        </Form.Item>

        <Form.Item
          label="规则名称"
          name="name"
          rules={[{ required: true, message: "请输入规则名称" }]}
        >
          <Input placeholder="如：水位超高告警" />
        </Form.Item>

        <Form.Item
          label="关联设备"
          name="device_id"
          rules={[{ required: true, message: "请选择关联设备" }]}
        >
          <Select
            showSearch
            optionFilterProp="label"
            placeholder="搜索并选择设备"
            options={deviceOptions.map((d) => ({ label: d.name, value: d.id }))}
          />
        </Form.Item>

        <div className="flex gap-4">
          <Form.Item label="严重级别" name="severity" className="flex-1">
            <Select>
              <Select.Option value="critical">严重</Select.Option>
              <Select.Option value="warning">警告</Select.Option>
              <Select.Option value="info">信息</Select.Option>
            </Select>
          </Form.Item>

          <Form.Item label="条件逻辑" name="logic" className="flex-1">
            <Select>
              <Select.Option value="and">全部满足 (AND)</Select.Option>
              <Select.Option value="or">任一满足 (OR)</Select.Option>
            </Select>
          </Form.Item>
        </div>

        <div className="mb-4">
          <div className="flex items-center justify-between mb-2">
            <span className="font-medium">
              告警条件
              {isLoadingConfig && selectedDeviceId && <Spin size="small" className="ml-2" />}
            </span>
            <Button type="dashed" size="small" icon={<PlusOutlined />} onClick={addCondition}>
              添加条件
            </Button>
          </div>
          {conditions.length === 0 && (
            <div className="text-gray-400 text-center py-4 border border-dashed rounded">
              暂无条件，请点击"添加条件"
            </div>
          )}
          {conditions.map((cond, index) => (
            <ConditionEditor
              key={`${index}-${cond.type}`}
              value={cond}
              elementOptions={elementOptions}
              onChange={(v) => updateCondition(index, v)}
              onRemove={() => removeCondition(index)}
            />
          ))}
        </div>

        <div className="flex gap-4">
          <Form.Item label="冷却时间(秒)" name="silence_duration" className="flex-1">
            <InputNumber min={0} max={86400} className="w-full" />
          </Form.Item>

          <Form.Item label="状态" name="status" className="flex-1">
            <Select>
              <Select.Option value="enabled">启用</Select.Option>
              <Select.Option value="disabled">禁用</Select.Option>
            </Select>
          </Form.Item>
        </div>

        <Form.Item label="备注" name="remark">
          <Input.TextArea rows={2} placeholder="可选备注" />
        </Form.Item>
      </Form>
    </Modal>
  );
}
