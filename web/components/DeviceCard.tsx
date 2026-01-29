/**
 * 设备卡片组件 - 用于展示设备实时数据
 */

import React from "react";

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

const calcWeightedLength = (str: string) => {
  let len = 0;
  for (const ch of str) {
    if (/[\u4e00-\u9fa5]/.test(ch)) len += 1.5;
    else len += 1;
  }
  return len;
};

const cardStyle: React.CSSProperties = {
  background: "#fff",
  borderRadius: 8,
  padding: "12px 14px",
  border: "1px solid #e8e8e8",
  boxShadow: "0 2px 8px rgba(0,0,0,.08)",
  display: "flex",
  flexDirection: "column",
  gap: 10,
  height: "100%", // 撑满父容器高度，实现等高卡片
};

const titleStyle: React.CSSProperties = {
  fontSize: 16,
  fontWeight: 700,
  display: "flex",
  alignItems: "center",
  gap: 8,
};

const subtitleStyle: React.CSSProperties = {
  fontSize: 12,
  color: "#888",
  marginTop: -4,
};

const dividerStyle: React.CSSProperties = {
  height: 1,
  background: "#f0f0f0",
};

const tableStyle: React.CSSProperties = {
  display: "flex",
  flexDirection: "column",
  flex: 1, // 占据剩余空间，让底部操作按钮固定在卡片底部
  justifyContent: "space-evenly", // 均匀分布要素行
  gap: 2, // 行间距
};

const itemStyle: React.CSSProperties = {
  display: "flex",
  fontSize: 14,
};

const labelStyle: React.CSSProperties = {
  fontWeight: 500,
  marginRight: 4,
  whiteSpace: "nowrap",
};

const valueStyle: React.CSSProperties = {
  flex: 1,
  wordBreak: "break-all",
};

const extraStyle: React.CSSProperties = {
  marginTop: 4,
  paddingTop: 6,
  borderTop: "1px solid #f5f5f5",
  fontSize: 13,
  color: "#666",
};

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

  return (
    <div style={cardStyle}>
      {/* 主标题 */}
      <div style={titleStyle}>{title}</div>

      {/* 副标题（可选） */}
      {subtitle && <div style={subtitleStyle}>{subtitle}</div>}

      {/* 分割线 */}
      <div style={dividerStyle} />

      {/* 内容区 */}
      <div style={tableStyle}>
        {rows.map((r, idx) => (
          <div
            key={idx}
            style={{ display: "grid", gap: 6, gridTemplateColumns: `repeat(${column}, 1fr)` }}
          >
            {r.map((it) => (
              <div key={it.key} style={{ ...itemStyle, gridColumnEnd: `span ${it.span}` }}>
                <span style={labelStyle}>{it.label}：</span>
                <span style={valueStyle}>{it.children}</span>
              </div>
            ))}
          </div>
        ))}
      </div>

      {/* bottom extra */}
      {extra && <div style={extraStyle}>{extra}</div>}
    </div>
  );
};

export default DeviceCard;
