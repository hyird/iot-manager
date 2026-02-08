/**
 * 设备卡片组件 - 用于展示设备实时数据
 */

import { Tooltip } from "antd";
import React, { useMemo } from "react";
import { calcWeightedLength } from "@/utils";

interface DeviceCardItem {
  key: React.Key;
  label: string;
  children: React.ReactNode;
  span?: number;
}

interface DeviceCardProps {
  title: React.ReactNode;
  subtitle?: React.ReactNode;
  items: DeviceCardItem[];
  column?: number;
  length?: number;
  extra?: React.ReactNode;
}

const DeviceCard: React.FC<DeviceCardProps> = ({
  title,
  subtitle,
  items,
  column = 2,
  length = 16,
  extra,
}) => {
  // Step 1: span 计算
  const processed = items.map((item) => {
    // 如果已经有 span，直接使用
    if (item.span !== undefined) {
      return { ...item, span: item.span };
    }

    const label = item.label || "";
    const value =
      typeof item.children === "string"
        ? item.children
        : typeof item.children === "number"
          ? String(item.children)
          : "";

    const total = calcWeightedLength(label) + calcWeightedLength(value);
    let span = Math.ceil(total / length);
    if (span > column) span = column;

    return { ...item, span };
  });

  // Step 2: 拆行
  const rows: DeviceCardItem[][] = [];
  let row: DeviceCardItem[] = [];
  let acc = 0;

  for (const it of processed) {
    if (acc + it.span! > column) {
      rows.push(row);
      row = [];
      acc = 0;
    }
    row.push(it);
    acc += it.span!;
  }
  if (row.length > 0) rows.push(row);

  const gridCols = useMemo(() => ({ gridTemplateColumns: `repeat(${column}, 1fr)` }), [column]);

  return (
    <div className="bg-white rounded-lg px-3.5 py-3 border border-gray-200 shadow-[0_2px_8px_rgba(0,0,0,0.08)] flex flex-col gap-2.5 h-full">
      {/* 主标题 */}
      <div className="text-base font-bold flex items-center gap-2">{title}</div>

      {/* 副标题（可选） */}
      {subtitle && <div className="text-xs text-gray-400 -mt-1">{subtitle}</div>}

      {/* 分割线 */}
      <div className="h-px bg-gray-100" />

      {/* 内容区 */}
      <div className="flex flex-col flex-1 justify-evenly gap-0.5">
        {rows.map((r, idx) => (
          <div key={idx} className="grid gap-1.5" style={gridCols}>
            {r.map((it) => (
              <div key={it.key} className="flex text-sm" style={{ gridColumn: `span ${it.span!}` }}>
                <span className="font-medium mr-1 whitespace-nowrap">{it.label}：</span>
                <span className="flex-1 truncate">
                  {typeof it.children === "string" || typeof it.children === "number" ? (
                    <Tooltip title={it.children}>{String(it.children)}</Tooltip>
                  ) : (
                    it.children
                  )}
                </span>
              </div>
            ))}
          </div>
        ))}
      </div>

      {/* bottom extra */}
      {extra && (
        <div className="mt-1 pt-1.5 border-t border-gray-100 text-[13px] text-gray-500">
          {extra}
        </div>
      )}
    </div>
  );
};

export default React.memo(DeviceCard);
