import {
  ArrowDownOutlined,
  ArrowLeftOutlined,
  ArrowRightOutlined,
  ArrowUpOutlined,
  ControlOutlined,
  MinusOutlined,
  PlusOutlined,
  StopOutlined,
} from "@ant-design/icons";
import { Button, Popover, Slider, Space, Tooltip, Typography } from "antd";
import { type ReactNode, useCallback, useEffect, useRef, useState } from "react";
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
  onSpeedChange: (speed: number) => void;
  onAction: (action: GB28181.PtzAction) => Promise<unknown>;
};

export function PtzPanel({ speed, disabled, onSpeedChange, onAction }: PtzPanelProps) {
  const [open, setOpen] = useState(false);
  const holdingRef = useRef<GB28181.PtzAction | null>(null);
  const onActionRef = useRef(onAction);

  useEffect(() => {
    onActionRef.current = onAction;
  }, [onAction]);

  const sendAction = useCallback((action: GB28181.PtzAction) => {
    void onActionRef.current(action).catch(() => undefined);
  }, []);

  const stopHolding = useCallback(() => {
    if (!holdingRef.current) return;
    holdingRef.current = null;
    sendAction("stop");
  }, [sendAction]);

  useEffect(() => {
    if (disabled) stopHolding();
  }, [disabled, stopHolding]);

  useEffect(
    () => () => {
      if (!holdingRef.current) return;
      holdingRef.current = null;
      sendAction("stop");
    },
    [sendAction]
  );

  const startHolding = useCallback(
    (action: GB28181.PtzAction) => {
      if (disabled || holdingRef.current) return;
      holdingRef.current = action;
      sendAction(action);
    },
    [disabled, sendAction]
  );

  const panel = (
    <div className="w-[240px] select-none space-y-3 rounded-lg bg-black/70 p-3 text-white shadow-xl backdrop-blur-md">
      <div className="flex items-center justify-between">
        <Text strong className="!text-white">
          云台控制
        </Text>
        <Text className="!text-white/70 text-xs">速度 {speed}</Text>
      </div>
      <Text className="!text-white/70 block text-center text-xs">
        按住方向或变倍按钮控制，松开即停止
      </Text>
      <div>
        <Slider
          min={1}
          max={255}
          value={speed}
          onChange={(value) => onSpeedChange(Number(value))}
        />
      </div>
      <div className="grid grid-cols-3 gap-2 w-[210px] mx-auto">
        {PTZ_ACTIONS.map((item) => (
          <Tooltip title={item.title} key={item.action}>
            <Button
              className={item.className}
              icon={item.icon}
              disabled={disabled}
              danger={item.action === "stop"}
              type={item.action === "stop" ? "primary" : "default"}
              onClick={
                item.action === "stop"
                  ? () => {
                      holdingRef.current = null;
                      sendAction("stop");
                    }
                  : undefined
              }
              onPointerDown={
                item.action === "stop"
                  ? undefined
                  : (event) => {
                      event.preventDefault();
                      startHolding(item.action);
                    }
              }
              onPointerUp={item.action === "stop" ? undefined : stopHolding}
              onPointerCancel={item.action === "stop" ? undefined : stopHolding}
              onPointerLeave={item.action === "stop" ? undefined : stopHolding}
            />
          </Tooltip>
        ))}
      </div>
      <Space.Compact block>
        <Button
          icon={<PlusOutlined />}
          disabled={disabled}
          onPointerDown={(event) => {
            event.preventDefault();
            startHolding("zoomin");
          }}
          onPointerUp={stopHolding}
          onPointerCancel={stopHolding}
          onPointerLeave={stopHolding}
        >
          变倍+
        </Button>
        <Button
          icon={<MinusOutlined />}
          disabled={disabled}
          onPointerDown={(event) => {
            event.preventDefault();
            startHolding("zoomout");
          }}
          onPointerUp={stopHolding}
          onPointerCancel={stopHolding}
          onPointerLeave={stopHolding}
        >
          变倍-
        </Button>
      </Space.Compact>
    </div>
  );

  return (
    <Popover
      trigger="click"
      placement="bottomRight"
      arrow={false}
      open={open}
      onOpenChange={(nextOpen) => {
        if (!nextOpen) stopHolding();
        setOpen(nextOpen);
      }}
      content={panel}
      styles={{
        container: {
          padding: 0,
          background: "transparent",
          boxShadow: "none",
        },
      }}
    >
      <Button
        type="primary"
        icon={<ControlOutlined />}
        disabled={disabled}
        className="!bg-black/65 !shadow-lg backdrop-blur hover:!bg-black/75"
      >
        云台
      </Button>
    </Popover>
  );
}
