import { CloseOutlined } from "@ant-design/icons";
import { Button, Col, Input, InputNumber, Row, Select } from "antd";
import type { Alert } from "@/types";

const CONDITION_TYPE_OPTIONS = [
  { label: "阈值", value: "threshold" },
  { label: "离线检测", value: "offline" },
  { label: "变化率", value: "rate_of_change" },
];

const OPERATOR_OPTIONS = [
  { label: ">", value: ">" },
  { label: ">=", value: ">=" },
  { label: "<", value: "<" },
  { label: "<=", value: "<=" },
  { label: "==", value: "==" },
  { label: "!=", value: "!=" },
];

const DIRECTION_OPTIONS = [
  { label: "任意", value: "any" },
  { label: "上升", value: "rise" },
  { label: "下降", value: "fall" },
];

interface ElementOption {
  value: string;
  label: string;
}

interface ConditionEditorProps {
  value: Alert.Condition;
  elementOptions: ElementOption[];
  onChange: (value: Alert.Condition) => void;
  onRemove: () => void;
}

export function ConditionEditor({
  value,
  elementOptions,
  onChange,
  onRemove,
}: ConditionEditorProps) {
  const update = (patch: Partial<Alert.Condition>) => {
    onChange({ ...value, ...patch });
  };

  const hasElements = elementOptions.length > 0;

  return (
    <div className="border border-gray-200 rounded-md p-3 mb-2 relative">
      <Button
        type="text"
        size="small"
        danger
        icon={<CloseOutlined />}
        onClick={onRemove}
        className="absolute top-1 right-1"
      />

      <Row gutter={[8, 8]}>
        <Col span={8}>
          <Select
            value={value.type}
            onChange={(type: Alert.ConditionType) => {
              if (type === "threshold") {
                onChange({ type, elementKey: "", operator: ">", value: "" });
              } else if (type === "offline") {
                onChange({ type, duration: 300 });
              } else {
                onChange({ type, elementKey: "", changeRate: "", changeDirection: "any" });
              }
            }}
            options={CONDITION_TYPE_OPTIONS}
            placeholder="条件类型"
            className="w-full"
          />
        </Col>

        {value.type === "threshold" && (
          <>
            <Col span={8}>
              {hasElements ? (
                <Select
                  value={value.elementKey || undefined}
                  onChange={(v: string) => update({ elementKey: v })}
                  options={elementOptions}
                  showSearch
                  optionFilterProp="label"
                  placeholder="选择要素"
                  className="w-full"
                />
              ) : (
                <Input
                  value={value.elementKey}
                  onChange={(e) => update({ elementKey: e.target.value })}
                  placeholder="要素标识"
                />
              )}
            </Col>
            <Col span={4}>
              <Select
                value={value.operator}
                onChange={(operator: Alert.Operator) => update({ operator })}
                options={OPERATOR_OPTIONS}
                className="w-full"
              />
            </Col>
            <Col span={4}>
              <Input
                value={value.value}
                onChange={(e) => update({ value: e.target.value })}
                placeholder="阈值"
              />
            </Col>
          </>
        )}

        {value.type === "offline" && (
          <Col span={16}>
            <InputNumber
              value={value.duration}
              onChange={(v) => update({ duration: v ?? 300 })}
              addonAfter="秒"
              min={60}
              max={86400}
              placeholder="离线超时时间"
              className="w-full"
            />
          </Col>
        )}

        {value.type === "rate_of_change" && (
          <>
            <Col span={8}>
              {hasElements ? (
                <Select
                  value={value.elementKey || undefined}
                  onChange={(v: string) => update({ elementKey: v })}
                  options={elementOptions}
                  showSearch
                  optionFilterProp="label"
                  placeholder="选择要素"
                  className="w-full"
                />
              ) : (
                <Input
                  value={value.elementKey}
                  onChange={(e) => update({ elementKey: e.target.value })}
                  placeholder="要素标识"
                />
              )}
            </Col>
            <Col span={4}>
              <Input
                value={value.changeRate}
                onChange={(e) => update({ changeRate: e.target.value })}
                placeholder="变化率%"
              />
            </Col>
            <Col span={4}>
              <Select
                value={value.changeDirection}
                onChange={(changeDirection: Alert.ChangeDirection) => update({ changeDirection })}
                options={DIRECTION_OPTIONS}
                className="w-full"
              />
            </Col>
          </>
        )}
      </Row>
    </div>
  );
}
