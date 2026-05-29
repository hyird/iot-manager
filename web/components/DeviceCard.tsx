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

type ProcessedDeviceCardItem = DeviceCardItem & { span: number };

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
const LABEL_VALUE_VISUAL_EXTRA_LENGTH = 2.5;

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

    const total =
      calcWeightedLength(label) + calcWeightedLength(value) + LABEL_VALUE_VISUAL_EXTRA_LENGTH;
    let span = Math.ceil(total / length);
    if (span < 1) span = 1;
    if (span > column) span = column;

    return { ...item, span };
  });

const buildItemRows = (items: ProcessedDeviceCardItem[], column: number) => {
  const rows: ProcessedDeviceCardItem[][] = [];
  let currentRow: ProcessedDeviceCardItem[] = [];
  let currentSpan = 0;

  for (const item of items) {
    if (currentRow.length > 0 && currentSpan + item.span > column) {
      rows.push(currentRow);
      currentRow = [];
      currentSpan = 0;
    }

    currentRow.push(item);
    currentSpan += item.span;

    if (currentSpan >= column) {
      rows.push(currentRow);
      currentRow = [];
      currentSpan = 0;
    }
  }

  if (currentRow.length > 0) {
    rows.push(currentRow);
  }

  return rows;
};

const renderRows = (
  items: DeviceCardItem[],
  column: number,
  length: number,
  compact = false,
  fillVertical = false
) => {
  const processed = buildProcessedItems(items, column, compact ? Math.max(length - 3, 14) : length);
  const gridCols = { gridTemplateColumns: `repeat(${column}, minmax(0, 1fr))` };
  const itemTextClass = compact ? "text-[12px] leading-[18px]" : "text-[13px] leading-[18px]";
  const inlineScalarItem = column >= 3;

  const renderItem = (it: ProcessedDeviceCardItem) => {
    const isScalar = typeof it.children === "string" || typeof it.children === "number";

    return (
      <div
        key={it.key}
        className={`min-w-0 rounded-md bg-white/80 px-1.5 py-1 shadow-[inset_0_0_0_1px_rgba(148,163,184,0.12)] ${itemTextClass}`}
        style={{ gridColumn: `span ${it.span}` }}
      >
        {isScalar && inlineScalarItem ? (
          <div className="flex min-w-0 items-baseline gap-1">
            <Tooltip title={it.label}>
              <div className="min-w-0 flex-1 truncate text-[11px] font-medium text-slate-500">
                {it.label}
              </div>
            </Tooltip>
            <Tooltip title={it.children}>
              <div className="max-w-[48%] shrink-0 truncate whitespace-nowrap font-semibold tabular-nums text-slate-950">
                {String(it.children)}
              </div>
            </Tooltip>
          </div>
        ) : (
          <>
            <Tooltip title={it.label}>
              <div className="min-w-0 truncate text-[11px] font-medium text-slate-500">
                {it.label}
              </div>
            </Tooltip>
            {isScalar ? (
              <Tooltip title={it.children}>
                <div className="min-w-0 truncate whitespace-nowrap font-semibold tabular-nums text-slate-950">
                  {String(it.children)}
                </div>
              </Tooltip>
            ) : (
              <div className="min-w-0 font-semibold text-slate-950">{it.children}</div>
            )}
          </>
        )}
      </div>
    );
  };

  if (fillVertical) {
    const rows = buildItemRows(processed, column);
    return (
      <div className="flex min-h-0 flex-1 flex-col gap-1.5">
        {rows.map((row, index) => (
          <div
            key={row.map((item) => item.key).join("-") || index}
            className="grid gap-1.5"
            style={gridCols}
          >
            {row.map(renderItem)}
          </div>
        ))}
      </div>
    );
  }

  return (
    <div className="grid gap-1.5" style={gridCols}>
      {processed.map(renderItem)}
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
    <div className="flex h-full flex-col gap-1.5 rounded-lg border border-slate-200 bg-white px-3.5 py-2.5 shadow-[0_8px_24px_rgba(15,23,42,0.08)] transition-shadow hover:shadow-[0_12px_28px_rgba(15,23,42,0.12)]">
      {/* 主标题 */}
      <div className="min-w-0 text-[15px] font-semibold leading-5 text-slate-950">{title}</div>

      {/* 副标题（可选） */}
      {subtitle && <div className="text-xs leading-5 text-slate-500">{subtitle}</div>}

      {/* 分割线 */}
      <div className="h-px bg-slate-100" />

      {/* 内容区 */}
      <div
        className={
          hasGroupSections
            ? "flex flex-1 flex-col gap-1"
            : "flex flex-1 flex-col justify-between gap-1"
        }
      >
        {hasGroupSections
          ? sections.map((section) => {
              const compact = section.items.length >= DENSE_SECTION_ITEM_COUNT;
              return (
                <section
                  key={section.key}
                  className="flex flex-col rounded-md border border-slate-100 bg-slate-50/70 px-2 py-1.5"
                >
                  <div className="mb-1.5 flex shrink-0 items-center gap-2">
                    <span className="rounded-full bg-white px-2 py-0 text-[11px] font-semibold leading-5 text-slate-600 shadow-sm">
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
        <div className="mt-0.5 border-t border-slate-100 pt-1.5 text-[11px] leading-4 text-slate-500">
          {extra}
        </div>
      )}
    </div>
  );
};

export default React.memo(DeviceCard);
