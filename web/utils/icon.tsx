import DynamicIcon from "@/components/DynamicIcon";
import type { ReactNode } from "react";

export function renderIcon(iconName?: string): ReactNode {
  if (!iconName) return undefined;
  return <DynamicIcon name={iconName} />;
}
