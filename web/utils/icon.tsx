import type { ReactNode } from "react";
import DynamicIcon from "@/components/DynamicIcon";

export function renderIcon(iconName?: string): ReactNode {
  if (!iconName) return undefined;
  return <DynamicIcon name={iconName} />;
}
