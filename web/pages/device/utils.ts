/**
 * 设备页面工具函数
 */

import dayjs from "dayjs";
import type { Device } from "@/types";

/** 默认在线超时时间（秒） */
export const DEFAULT_ONLINE_TIMEOUT = 300;

/** 获取默认时间范围（最近一周） */
export const getDefaultTimeRange = () => [
  dayjs().subtract(7, "day").startOf("day"),
  dayjs().endOf("day"),
];

/**
 * 判断设备在线状态
 * 优先使用后端返回的 connected 字段（基于实际 TCP 连接状态），
 * 若 connected 未提供则回退到上报时间判断。
 */
export const isOnline = (connected?: boolean, reportTime?: string, onlineTimeout?: number) => {
  // 优先使用后端的连接状态
  if (connected !== undefined) return connected;

  // 回退：基于上报时间判断
  if (!reportTime) return false;
  const reportTs = new Date(reportTime).getTime();
  if (reportTs === 0 || Number.isNaN(reportTs)) return false;

  const threshold = (onlineTimeout || DEFAULT_ONLINE_TIMEOUT) * 1000;
  return Date.now() - reportTs < threshold;
};

/** 格式化上报时间 */
export const formatReportTime = (reportTime?: string) => {
  if (!reportTime) return "--";
  const t = dayjs(reportTime);
  if (!t.isValid()) return "--";
  return t.format("YYYY-MM-DD HH:mm:ss");
};

const NUMERIC_TEXT_RE = /^-?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;

/** 按小数位格式化要素值 */
export const formatElementValue = (
  value: string | number | null | undefined,
  decimals?: number
) => {
  if (value === null || value === undefined || value === "") return "--";

  if (typeof decimals === "number" && decimals >= 0) {
    const numericValue =
      typeof value === "number"
        ? value
        : NUMERIC_TEXT_RE.test(value.trim())
          ? Number.parseFloat(value)
          : Number.NaN;

    if (Number.isFinite(numericValue)) {
      return numericValue.toFixed(decimals);
    }
  }

  return String(value);
};

/** 解析位映射字典配置 */
export const parseBitMapping = (
  value: string | number,
  dictConfig: NonNullable<Device.Element["dictConfig"]>
) => {
  let numValue: number;
  if (typeof value === "string") {
    numValue =
      value.startsWith("0x") || /^[0-9A-Fa-f]+$/.test(value)
        ? parseInt(value, 16)
        : parseInt(value, 10);
  } else {
    numValue = value;
  }

  if (Number.isNaN(numValue)) return [];

  const matchedLabels: string[] = [];
  for (const item of dictConfig.items) {
    if (!item || typeof item !== "object") continue;

    const bitIndex = parseInt(item.key, 10);
    if (Number.isNaN(bitIndex) || bitIndex < 0 || bitIndex > 31) continue;

    if (item.dependsOn?.conditions && item.dependsOn.conditions.length > 0) {
      const { operator, conditions } = item.dependsOn;
      let anyMet = false;
      let allMet = true;

      for (const dep of conditions) {
        if (!dep || typeof dep !== "object" || dep.bitIndex === undefined) {
          allMet = false;
          continue;
        }

        const depBitIndex = parseInt(dep.bitIndex, 10);
        if (Number.isNaN(depBitIndex) || depBitIndex < 0 || depBitIndex > 31) {
          allMet = false;
          continue;
        }

        const depBitValue = (numValue >> depBitIndex) & 1;
        const depExpectedValue = dep.bitValue === "1" ? 1 : 0;
        const conditionMet = depBitValue === depExpectedValue;

        if (conditionMet) anyMet = true;
        if (!conditionMet) allMet = false;
      }

      const dependencyMet = operator === "OR" ? anyMet : allMet;
      if (!dependencyMet) continue;
    }

    const bitValue = (numValue >> bitIndex) & 1;
    const triggerValue = item.value || "1";
    const shouldTrigger =
      (triggerValue === "1" && bitValue === 1) || (triggerValue === "0" && bitValue === 0);

    if (shouldTrigger) {
      matchedLabels.push(item.label);
    }
  }

  return matchedLabels;
};

/** 解析要素显示值（统一字典映射逻辑） */
export const resolveElementDisplay = (
  el: Device.Element | undefined,
  decimals?: number
): { type: "text"; value: string } | { type: "bits"; labels: string[] } => {
  if (!el) return { type: "text", value: "---" };

  if (el.dictConfig && el.value !== null && el.value !== undefined && el.value !== "") {
    if (el.dictConfig.mapType === "VALUE") {
      const rawValue = String(el.value);
      const matchedItem = el.dictConfig.items.find(
        (item) => item && typeof item === "object" && item.key === rawValue
      );
      if (matchedItem) return { type: "text", value: matchedItem.label };
      const displayValue = formatElementValue(el.value, decimals ?? el.decimals);
      return {
        type: "text",
        value:
          displayValue === "--"
            ? displayValue
            : el.unit
              ? `${displayValue} ${el.unit}`
              : displayValue,
      };
    } else if (el.dictConfig.mapType === "BIT") {
      const matchedLabels = parseBitMapping(el.value, el.dictConfig);
      if (matchedLabels.length > 0) return { type: "bits", labels: matchedLabels };
      const displayValue = formatElementValue(el.value, decimals ?? el.decimals);
      return {
        type: "text",
        value:
          displayValue === "--"
            ? displayValue
            : el.unit
              ? `${displayValue} ${el.unit}`
              : displayValue,
      };
    }
  }

  const displayValue = formatElementValue(el.value, decimals ?? el.decimals);
  return {
    type: "text",
    value:
      displayValue === "--" ? displayValue : el.unit ? `${displayValue} ${el.unit}` : displayValue,
  };
};

// 从公共 utils 导入，避免重复定义
export { calcWeightedLength } from "@/utils";

/** 分隔线 Tailwind 类名 */
export const separatorClass = "inline-block h-3.5 mx-1 border-l border-gray-300";
