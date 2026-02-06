/**
 * 指令下发 Popover 内容组件
 */

import { App, Button, Checkbox, Flex, Input } from "antd";
import { useCallback, useState } from "react";
import { useDeviceCommand } from "@/services";
import type { Device } from "@/types";
import { isOnline } from "./utils";

interface CommandElement {
  _key: string;
  elementId: number;
  name: string;
  value: string;
  unit?: string;
  options?: Device.DownFuncElement["options"];
}

interface CommandPopoverProps {
  device: Device.RealTimeData;
  func: Device.DownFunc;
  onClose: () => void;
}

const CommandPopover = ({ device, func, onClose }: CommandPopoverProps) => {
  const { message } = App.useApp();
  const commandMutation = useDeviceCommand();

  const [elements, setElements] = useState<CommandElement[]>(() =>
    (func.elements || []).map((el) => ({
      ...el,
      _key: String(el.elementId ?? el.name),
      value: el.value ?? "",
    }))
  );
  const [selectedKeys, setSelectedKeys] = useState<string[]>(() =>
    (func.elements || []).map((el) => String(el.elementId ?? el.name))
  );

  const checkOnline = useCallback(() => {
    const online = isOnline(device.lastHeartbeatTime, device.reportTime, device.online_timeout);
    if (!online) {
      message.error("设备离线，无法下发指令");
      return false;
    }
    return true;
  }, [device.lastHeartbeatTime, device.reportTime, device.online_timeout, message]);

  const checkLinkId = useCallback(() => {
    if (!device.linkId) {
      message.error("缺少链路ID");
      return false;
    }
    return true;
  }, [device.linkId, message]);

  const handleSend = useCallback(() => {
    const toSend = elements.filter((el) => selectedKeys.includes(el._key));
    if (!toSend.length) {
      message.warning("请至少选择一个要素");
      return;
    }
    if (!checkLinkId() || !checkOnline()) return;

    commandMutation.mutate(
      {
        linkId: device.linkId!,
        payload: {
          deviceCode: device.code,
          funcCode: func.funcCode,
          elements: toSend.map((el) => ({ elementId: el.elementId, value: el.value })),
        },
      },
      { onSuccess: onClose }
    );
  }, [
    elements,
    selectedKeys,
    device,
    func,
    commandMutation,
    onClose,
    checkOnline,
    checkLinkId,
    message,
  ]);

  const handlePresetClick = useCallback(
    (el: CommandElement, optValue: string) => {
      if (!checkLinkId() || !checkOnline()) return;
      commandMutation.mutate({
        linkId: device.linkId!,
        payload: {
          deviceCode: device.code,
          funcCode: func.funcCode,
          elements: [{ elementId: el.elementId, value: optValue }],
        },
      });
    },
    [device, func, commandMutation, checkOnline, checkLinkId]
  );

  if (!elements.length) return <div className="p-3">暂无可下发要素</div>;

  return (
    <div className="max-w-[360px]">
      <div className="mb-2">
        <div>
          设备：{device.deviceName}（{device.code}）
        </div>
        <div className="text-xs text-gray-400">
          指令：{func.name}（{func.funcCode}）
        </div>
      </div>
      <div className="max-h-[260px] overflow-y-auto pr-1 mb-2">
        {elements.map((el) => {
          const key = el._key;
          const checked = selectedKeys.includes(key);
          const hasOptions = el.options && el.options.length > 0;
          return (
            <div key={key} className={`mb-2 pb-2 ${hasOptions ? "border-b border-gray-100" : ""}`}>
              <Flex align="center" className={hasOptions ? "mb-1.5" : ""}>
                <Checkbox
                  checked={checked}
                  onChange={(e) =>
                    setSelectedKeys((prev) =>
                      e.target.checked ? [...prev, key] : prev.filter((k) => k !== key)
                    )
                  }
                />
                <span className="flex-1 mx-1.5">
                  {el.name}
                  {el.unit ? `（${el.unit}）` : ""}
                </span>
                <Input
                  size="small"
                  className="!w-[120px]"
                  value={el.value}
                  placeholder={hasOptions ? "或手动输入" : ""}
                  onChange={(e) =>
                    setElements((prev) =>
                      prev.map((item) =>
                        item._key === key ? { ...item, value: e.target.value } : item
                      )
                    )
                  }
                />
              </Flex>
              {hasOptions && (
                <Flex wrap gap={6} className="ml-[26px]">
                  <span className="text-xs text-gray-400 mr-1">预设值：</span>
                  {el.options!.map((opt) => (
                    <Button
                      key={opt.value}
                      size="small"
                      type="primary"
                      ghost
                      loading={commandMutation.isPending}
                      onClick={() => handlePresetClick(el, opt.value)}
                    >
                      {opt.label}
                    </Button>
                  ))}
                </Flex>
              )}
            </div>
          );
        })}
      </div>
      <Flex justify="flex-end" gap={8}>
        <Button size="small" onClick={onClose}>
          取消
        </Button>
        <Button
          size="small"
          type="primary"
          loading={commandMutation.isPending}
          disabled={!selectedKeys.length}
          onClick={handleSend}
        >
          下发
        </Button>
      </Flex>
    </div>
  );
};

export default CommandPopover;
