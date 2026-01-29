import * as AntdIcons from "@ant-design/icons";
import type { ComponentType } from "react";
import type { AntdIconProps } from "@ant-design/icons/lib/components/AntdIcon";
// 图标组件映射（排除非图标导出）
const iconComponents = Object.fromEntries(
  Object.entries(AntdIcons).filter(
    ([name]) => name.endsWith("Outlined") || name.endsWith("Filled") || name.endsWith("TwoTone")
  )
) as Record<string, ComponentType<AntdIconProps>>;

interface DynamicIconProps {
  name?: string;
  style?: React.CSSProperties;
  className?: string;
}

export default function DynamicIcon({ name, ...props }: DynamicIconProps) {
  if (!name) return null;

  const IconComp = iconComponents[name];

  if (!IconComp) {
    if (import.meta.env.DEV) {
      console.warn(`[DynamicIcon] Icon "${name}" not found in @ant-design/icons`);
    }
    return null;
  }
  return <IconComp {...props} />;
}
