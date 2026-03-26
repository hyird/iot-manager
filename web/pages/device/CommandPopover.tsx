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
  options?: Device.CommandOperationElement["options"];
  dataType?: string;
  size?: number;
  encode?: string;
  length?: number;
  digits?: number;
}

interface CommandPopoverProps {
  device: Device.RealTimeData;
  func: Device.CommandOperation;
  onClose: () => void;
}

/** 整数类型的值范围 */
const INTEGER_RANGES: Record<string, [bigint, bigint]> = {
  INT8: [-128n, 127n],
  UINT8: [0n, 255n],
  INT16: [-32768n, 32767n],
  UINT16: [0n, 65535n],
  INT32: [-2147483648n, 2147483647n],
  UINT32: [0n, 4294967295n],
  INT64: [-9223372036854775808n, 9223372036854775807n],
  UINT64: [0n, 18446744073709551615n],
};

const HEX_VALUE_PATTERN = /^[0-9a-fA-F]+$/;
const INTEGER_VALUE_PATTERN = /^[+-]?\d+$/;

const parseBigIntStrict = (value: string): bigint | null => {
  if (!INTEGER_VALUE_PATTERN.test(value)) return null;
  try {
    return BigInt(value);
  } catch {
    return null;
  }
};

/** 校验单个要素的值 */
const validateValue = (el: CommandElement): string | null => {
  const value = el.value?.trim();
  if (!value) return `「${el.name}」值不能为空`;

  if (el.dataType === "BOOL") {
    if (value !== "0" && value !== "1") return `「${el.name}」BOOL 类型只能输入 0 或 1`;
    return null;
  }

  if (el.dataType) {
    if (el.dataType === "STRING") {
      if (typeof el.size === "number" && el.size > 0 && value.length > el.size) {
        return `「${el.name}」STRING 长度不能超过 ${el.size} 字节`;
      }
      return null;
    }

    const intRange = INTEGER_RANGES[el.dataType];
    if (intRange) {
      const parsed = parseBigIntStrict(value);
      if (parsed == null) return `「${el.name}」请输入有效整数`;
      if (parsed < intRange[0] || parsed > intRange[1]) {
        return `「${el.name}」${el.dataType} 范围 ${intRange[0].toString()} ~ ${intRange[1].toString()}`;
      }
      return null;
    }

    const num = Number(value);
    if (!Number.isFinite(num)) return `「${el.name}」请输入有效数字`;

    if ((el.dataType === "FLOAT32" || el.dataType === "FLOAT") && !Number.isFinite(Math.fround(num))) {
      return `「${el.name}」${el.dataType} 值超出范围`;
    }
    if ((el.dataType === "DOUBLE" || el.dataType === "LREAL") && !Number.isFinite(num)) {
      return `「${el.name}」${el.dataType} 值不合法`;
    }
    return null;
  }

  if (el.encode === "BCD") {
    const num = Number(value);
    if (!Number.isFinite(num)) return `「${el.name}」BCD 编码只能输入数字`;

    const digits = Math.max(0, Math.min(8, el.digits ?? 0));
    const length = Math.max(1, el.length ?? 1);
    const scaled = Math.round(Math.abs(num) * 10 ** digits);
    if (scaled >= 10 ** (length * 2)) {
      return `「${el.name}」BCD 编码长度超出 ${length} 字节`;
    }
    return null;
  }

  if (el.encode) {
    if (!HEX_VALUE_PATTERN.test(value)) {
      return `「${el.name}」${el.encode} 编码只能输入十六进制字符`;
    }
    if (typeof el.length === "number" && el.length > 0 && value.length > el.length * 2) {
      return `「${el.name}」${el.encode} 编码长度不能超过 ${el.length} 字节`;
    }
    return null;
  }

  const num = Number(value);
  if (Number.isNaN(num)) return `「${el.name}」请输入有效数字`;
  return null;
};

const CommandPopover = ({ device, func, onClose }: CommandPopoverProps) => {
  const { message, modal } = App.useApp();
  const commandMutation = useDeviceCommand();
  const isS7SingleSelect = device.protocol_type === "S7";

  const [elements, setElements] = useState<CommandElement[]>(() =>
    (func.elements || []).map((el) => ({
      ...el,
      _key: String(el.elementId ?? el.name),
      value: el.value ?? "",
    }))
  );
  const [selectedKeys, setSelectedKeys] = useState<string[]>([]);

  const checkOnline = useCallback((): Promise<boolean> => {
    const online = isOnline(device.connected, device.reportTime, device.online_timeout);
    if (!online) {
      return new Promise((resolve) => {
        modal.confirm({
          title: "设备当前离线",
          content: (
            <Alert
              message="设备离线，指令可能无法送达"
              type="warning"
              showIcon
              className="!mt-2"
            />
          ),
          okText: "仍然下发",
          cancelText: "取消",
          onOk: () => resolve(true),
          onCancel: () => resolve(false),
        });
      });
    }
    return Promise.resolve(true);
  }, [device.connected, device.reportTime, device.online_timeout, modal]);

  const checkLinkId = useCallback(() => {
    if (device.link_id == null && !device.agent_id) {
      message.error("缺少链路ID");
      return false;
    }
    return true;
  }, [device.link_id, device.agent_id, message]);

  const handleSend = useCallback(async () => {
    const toSend = elements.filter((el) => selectedKeys.includes(el._key));
    if (!toSend.length) {
      message.warning(isS7SingleSelect ? "请选择一个要素" : "请至少选择一个要素");
      return;
    }
    if (isS7SingleSelect && toSend.length > 1) {
      message.warning("S7 写寄存器一次只能选择一个要素");
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

    const linkId = device.link_id ?? 0;

    commandMutation.mutate(
      {
        linkId,
        payload: {
          deviceCode: device.device_code ?? "",
          deviceId: device.id,
          elements: toSend.map((el) => ({ elementId: el.elementId, value: el.value })),
        },
      },
      { onSuccess: onClose }
    );
  }, [
    elements,
    selectedKeys,
    device,
    commandMutation,
    onClose,
    checkOnline,
    checkLinkId,
    isS7SingleSelect,
    message,
  ]);

  const handlePresetClick = useCallback(
    async (el: CommandElement, optValue: string) => {
      if (!checkLinkId() || !(await checkOnline())) return;
      const linkId = device.link_id ?? 0;
      commandMutation.mutate({
        linkId,
        payload: {
          deviceCode: device.device_code ?? "",
          deviceId: device.id,
          elements: [{ elementId: el.elementId, value: optValue }],
        },
      });
    },
    [device, commandMutation, checkOnline, checkLinkId]
  );

  if (!elements.length) return <div className="p-3">暂无可下发要素</div>;

  return (
    <div className="max-w-[360px]">
      <div className="mb-2">
        <div>
          设备：{device.name}（{device.device_code}）
        </div>
        <div className="text-xs text-gray-400">指令：{func.name}</div>
        {isS7SingleSelect && (
          <div className="text-xs text-gray-400">S7 写寄存器一次只能选择一个要素</div>
        )}
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
                      e.target.checked
                        ? (isS7SingleSelect ? [key] : [...prev, key])
                        : prev.filter((k) => k !== key)
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
                  {el.options?.map((opt) => (
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
          disabled={!selectedKeys.length || (isS7SingleSelect && selectedKeys.length > 1)}
          onClick={handleSend}
        >
          下发
        </Button>
      </Flex>
    </div>
  );
};

export default CommandPopover;
