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
const DENSE_SECTION_ITEM_COUNT = 8;

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

const renderRows = (items: DeviceCardItem[], column: number, length: number, compact = false) => {
  const processed = buildProcessedItems(items, column, compact ? Math.max(length - 3, 14) : length);
  const gridCols = { gridTemplateColumns: `repeat(${column}, minmax(0, 1fr))` };
  const itemTextClass = compact ? "text-[12px] leading-5" : "text-[13px] leading-6";

  return (
    <div className="grid gap-x-3 gap-y-0.5" style={gridCols}>
      {processed.map((it) => (
        <div
          key={it.key}
          className={`min-w-0 flex items-baseline ${itemTextClass}`}
          style={{ gridColumn: `span ${it.span!}` }}
        >
          <Tooltip title={it.label}>
            <span className="min-w-0 truncate font-medium text-slate-700">{it.label}：</span>
          </Tooltip>
          {typeof it.children === "string" || typeof it.children === "number" ? (
            <Tooltip title={it.children}>
              <span className="ml-1 shrink-0 whitespace-nowrap font-medium tabular-nums text-slate-950">
                {String(it.children)}
              </span>
            </Tooltip>
          ) : (
            <span className="ml-1 min-w-0 flex-1">{it.children}</span>
          )}
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
    <div className="bg-white rounded-lg px-3.5 py-2 border border-gray-200 shadow-[0_2px_8px_rgba(0,0,0,0.08)] flex flex-col gap-1.5 h-full">
      {/* 主标题 */}
      <div className="min-w-0 text-sm font-bold flex items-center gap-1.5 leading-4.5">{title}</div>

      {/* 副标题（可选） */}
      {subtitle && <div className="text-[10px] text-gray-400 -mt-1 leading-4">{subtitle}</div>}

      {/* 分割线 */}
      <div className="h-px bg-gray-100" />

      {/* 内容区 */}
      <div className="flex flex-col flex-1 justify-between gap-1.5">
        {hasGroupSections
          ? sections.map((section) => {
              const compact = section.items.length >= DENSE_SECTION_ITEM_COUNT;
              return (
                <section
                  key={section.key}
                  className="rounded-md border border-slate-100 bg-slate-50/60 px-2 py-1"
                >
                  <div className="mb-1 flex items-center gap-1.5">
                    <span className="rounded-full bg-white px-1.5 py-0.5 text-[10px] font-semibold text-slate-500 shadow-sm">
                      {section.label}
                    </span>
                    <span className="h-px flex-1 bg-slate-100" />
                  </div>
                  {renderRows(section.items, column, length, compact)}
                </section>
              );
            })
          : renderRows(items, column, length)}
      </div>

      {/* bottom extra */}
      {extra && (
        <div className="mt-0.5 pt-0.5 border-t border-gray-100 text-[11px] leading-4 text-gray-500">
          {extra}
        </div>
      )}
    </div>
  );
};

export default React.memo(DeviceCard);
