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
  group?: string;
}

interface DeviceCardSection {
  key: string;
  label: string;
  items: DeviceCardItem[];
}

interface DeviceCardProps {
  title: React.ReactNode;
  subtitle?: React.ReactNode;
  items: DeviceCardItem[];
  column?: number;
  length?: number;
  extra?: React.ReactNode;
}

const normalizeGroupName = (group?: string) => group?.trim() || "";

const UNGROUPED_GROUP_KEY = "__ungrouped__";

const buildSections = (items: DeviceCardItem[]): DeviceCardSection[] => {
  const sectionMap = new Map<string, DeviceCardSection>();

  for (const item of items) {
    const rawGroup = normalizeGroupName(item.group);
    const key = rawGroup || UNGROUPED_GROUP_KEY;
    const label = rawGroup || "未分组";
    const current = sectionMap.get(key);

    if (current) {
      current.items.push(item);
      continue;
    }

    sectionMap.set(key, {
      key,
      label,
      items: [item],
    });
  }

  return Array.from(sectionMap.values());
};

const buildProcessedItems = (items: DeviceCardItem[], column: number, length: number) =>
  items.map((item) => {
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

const renderRows = (items: DeviceCardItem[], column: number, length: number) => {
  const processed = buildProcessedItems(items, column, length);
  const rows: DeviceCardItem[][] = [];
  let row: DeviceCardItem[] = [];
  let acc = 0;

  for (const item of processed) {
    if (acc + item.span! > column) {
      rows.push(row);
      row = [];
      acc = 0;
    }
    row.push(item);
    acc += item.span!;
  }

  if (row.length > 0) {
    rows.push(row);
  }

  const gridCols = { gridTemplateColumns: `repeat(${column}, 1fr)` };

  return (
    <div className="flex flex-col gap-0.5">
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
  );
};

const DeviceCard: React.FC<DeviceCardProps> = ({
  title,
  subtitle,
  items,
  column = 2,
  length = 16,
  extra,
}) => {
  const sections = useMemo(() => buildSections(items), [items]);
  const hasGroupSections = useMemo(
    () => sections.some((section) => section.key !== UNGROUPED_GROUP_KEY),
    [sections]
  );

  return (
    <div className="bg-white rounded-lg px-3.5 py-2.5 border border-gray-200 shadow-[0_2px_8px_rgba(0,0,0,0.08)] flex flex-col gap-2 h-full">
      {/* 主标题 */}
      <div className="text-sm font-bold flex items-center gap-2 leading-5">{title}</div>

      {/* 副标题（可选） */}
      {subtitle && <div className="text-[11px] text-gray-400 -mt-0.5">{subtitle}</div>}

      {/* 分割线 */}
      <div className="h-px bg-gray-100" />

      {/* 内容区 */}
      <div className="flex flex-col flex-1 gap-2.5">
        {hasGroupSections
          ? sections.map((section) => (
              <section
                key={section.key}
                className="rounded-md border border-slate-100 bg-slate-50/60 px-2.5 py-1.5"
              >
                <div className="mb-1.5 flex items-center gap-2">
                  <span className="rounded-full bg-white px-2 py-0.5 text-[11px] font-semibold text-slate-500 shadow-sm">
                    {section.label}
                  </span>
                  <span className="h-px flex-1 bg-slate-100" />
                </div>
                {renderRows(section.items, column, length)}
              </section>
            ))
          : renderRows(items, column, length)}
      </div>

      {/* bottom extra */}
      {extra && (
        <div className="mt-0.5 pt-1 border-t border-gray-100 text-[12px] text-gray-500">
          {extra}
        </div>
      )}
    </div>
  );
};

export default React.memo(DeviceCard);
