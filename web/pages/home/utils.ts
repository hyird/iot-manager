/**
 * 首页工具函数（纯函数，无 JSX）
 */

export function formatUptime(seconds: number): string {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);

  if (days > 0) return `${days}天 ${hours}小时`;
  if (hours > 0) return `${hours}小时 ${minutes}分钟`;
  return `${minutes}分钟`;
}

export function formatMemory(mb: number): string {
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${Math.round(mb)} MB`;
}

export function formatDisk(gb: number): string {
  return `${gb.toFixed(1)} GB`;
}

export function formatBytes(bytes: number): string {
  if (bytes >= 1073741824) return `${(bytes / 1073741824).toFixed(2)} GB`;
  if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${bytes} B`;
}

export function formatNumber(n: number): string {
  if (n >= 1000000) return `${(n / 1000000).toFixed(1)}M`;
  if (n >= 1000) return `${(n / 1000).toFixed(1)}K`;
  return String(n);
}

/** 根据百分比返回状态颜色 */
export function getStatusColor(percent: number, reverse = false): string {
  if (reverse) {
    if (percent > 90) return "#52c41a";
    if (percent > 75) return "#faad14";
    return "#ff4d4f";
  }
  if (percent > 90) return "#ff4d4f";
  if (percent > 75) return "#faad14";
  return "#52c41a";
}
