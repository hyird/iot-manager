/**
 * 指令下发 Popover 内容组件
 */

import { Alert, App, Button, Checkbox, Flex, Input } from "antd";
import { useCallback, useState } from "react";
import { useDeviceCommand } from "@/services";
import type { Device } from "@/types";
import { isOnline } from "./utils";

interface CommandElement {
  _key: string;
  elementId: string;
  name: string;
  value: string;
  unit?: string;
  options?: Device.DownFuncElement["options"];
  dataType?: string;
}

interface CommandPopoverProps {
  device: Device.RealTimeData;
  func: Device.DownFunc;
  onClose: () => void;
}

/** Modbus 数据类型的值范围 */
const MODBUS_RANGES: Record<string, [number, number]> = {
  INT16: [-32768, 32767],
  UINT16: [0, 65535],
  INT32: [-2147483648, 2147483647],
  UINT32: [0, 4294967295],
};

/** 校验单个要素的值 */
const validateValue = (el: CommandElement): string | null => {
  const value = el.value?.trim();
  if (!value) return `「${el.name}」值不能为空`;

  if (el.dataType === "BOOL") {
    if (value !== "0" && value !== "1") return `「${el.name}」BOOL 类型只能输入 0 或 1`;
    return null;
  }

  const num = Number(value);
  if (Number.isNaN(num)) return `「${el.name}」请输入有效数字`;

  if (el.dataType) {
    const range = MODBUS_RANGES[el.dataType];
    if (range && (num < range[0] || num > range[1])) {
      return `「${el.name}」${el.dataType} 范围 ${range[0]} ~ ${range[1]}`;
    }
  }

  return null;
};

const CommandPopover = ({ device, func, onClose }: CommandPopoverProps) => {
  const { message, modal } = App.useApp();
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

  const checkOnline = useCallback((): Promise<boolean> => {
    const online = isOnline(device.lastHeartbeatTime, device.reportTime, device.online_timeout);
    if (!online) {
      return new Promise((resolve) => {
        modal.confirm({
          title: "设备当前离线",
          content: (
            <Alert
              message="指令将进入队列，待设备重新上线后自动执行"
              type="warning"
              showIcon
              className="!mt-2"
            />
          ),
          okText: "继续下发",
          cancelText: "取消",
          onOk: () => resolve(true),
          onCancel: () => resolve(false),
        });
      });
    }
    return Promise.resolve(true);
  }, [device.lastHeartbeatTime, device.reportTime, device.online_timeout, modal]);

  const checkLinkId = useCallback(() => {
    if (!device.link_id) {
      message.error("缺少链路ID");
      return false;
    }
    return true;
  }, [device.link_id, message]);

  const handleSend = useCallback(async () => {
    const toSend = elements.filter((el) => selectedKeys.includes(el._key));
    if (!toSend.length) {
      message.warning("请至少选择一个要素");
      return;
    }
    for (const el of toSend) {
      const error = validateValue(el);
      if (error) {
        message.error(error);
        return;
      }
    }
    if (!checkLinkId() || !(await checkOnline())) return;

    commandMutation.mutate(
      {
        linkId: device.link_id!,
        payload: {
          deviceCode: device.device_code ?? "",
          deviceId: device.id,
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
    async (el: CommandElement, optValue: string) => {
      if (!checkLinkId() || !(await checkOnline())) return;
      commandMutation.mutate({
        linkId: device.link_id!,
        payload: {
          deviceCode: device.device_code ?? "",
          deviceId: device.id,
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
          设备：{device.name}（{device.device_code}）
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
