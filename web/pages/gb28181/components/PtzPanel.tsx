import {
  ArrowDownOutlined,
  ArrowLeftOutlined,
  ArrowRightOutlined,
  ArrowUpOutlined,
  MinusOutlined,
  PlusOutlined,
  StopOutlined,
} from "@ant-design/icons";
import { Button, Slider, Space, Tooltip, Typography } from "antd";
import type { ReactNode } from "react";
import type { GB28181 } from "@/types";

const { Text } = Typography;

const PTZ_ACTIONS: Array<{
  action: GB28181.PtzAction;
  title: string;
  icon: ReactNode;
  className?: string;
}> = [
  { action: "up", title: "上", icon: <ArrowUpOutlined />, className: "col-start-2" },
  { action: "left", title: "左", icon: <ArrowLeftOutlined />, className: "col-start-1" },
  { action: "stop", title: "停止", icon: <StopOutlined />, className: "col-start-2" },
  { action: "right", title: "右", icon: <ArrowRightOutlined />, className: "col-start-3" },
  { action: "down", title: "下", icon: <ArrowDownOutlined />, className: "col-start-2" },
];

type PtzPanelProps = {
  speed: number;
  disabled: boolean;
  loading: boolean;
  onSpeedChange: (speed: number) => void;
  onAction: (action: GB28181.PtzAction) => void;
};

export function PtzPanel({ speed, disabled, loading, onSpeedChange, onAction }: PtzPanelProps) {
  return (
    <div className="space-y-4 xl:border-l xl:pl-4">
      <div className="flex items-center justify-between">
        <Text strong>云台</Text>
        <Text type="secondary" className="text-xs">
          速度 {speed}
        </Text>
      </div>
      <div>
        <Slider min={1} max={255} value={speed} onChange={(value) => onSpeedChange(Number(value))} />
      </div>
      <div className="grid grid-cols-3 gap-2 w-[210px] mx-auto">
        {PTZ_ACTIONS.map((item) => (
          <Tooltip title={item.title} key={item.action}>
            <Button
              className={item.className}
              icon={item.icon}
              disabled={disabled}
              loading={loading}
              onClick={() => onAction(item.action)}
            />
          </Tooltip>
        ))}
      </div>
      <Space.Compact block>
        <Button
          icon={<PlusOutlined />}
          disabled={disabled}
          loading={loading}
          onClick={() => onAction("zoomin")}
        >
          变倍+
        </Button>
        <Button
          icon={<MinusOutlined />}
          disabled={disabled}
          loading={loading}
          onClick={() => onAction("zoomout")}
        >
          变倍-
        </Button>
      </Space.Compact>
    </div>
  );
}
