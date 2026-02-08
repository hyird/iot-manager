import type { ComponentType } from "react";
import { useEffect, useState } from "react";

interface DynamicIconProps {
  name?: string;
  style?: React.CSSProperties;
  className?: string;
}

type IconComp = ComponentType<{ style?: React.CSSProperties; className?: string }>;

// 全量图标模块缓存（首次使用时动态加载，之后直接复用）
let iconMap: Record<string, IconComp> | null = null;
let loadPromise: Promise<Record<string, IconComp>> | null = null;

function loadIcons(): Promise<Record<string, IconComp>> {
  if (iconMap) return Promise.resolve(iconMap);
  if (!loadPromise) {
    loadPromise = import("@ant-design/icons").then((mod) => {
      iconMap = Object.fromEntries(
        Object.entries(mod).filter(
          ([n]) => n.endsWith("Outlined") || n.endsWith("Filled") || n.endsWith("TwoTone")
        )
      ) as Record<string, IconComp>;
      return iconMap;
    });
  }
  return loadPromise;
}

export default function DynamicIcon({ name, ...props }: DynamicIconProps) {
  const [Icon, setIcon] = useState<IconComp | null>(() =>
    name && iconMap ? (iconMap[name] ?? null) : null
  );

  useEffect(() => {
    if (!name) return;

    if (iconMap) {
      setIcon(() => iconMap![name] ?? null);
      return;
    }

    loadIcons().then((map) => {
      setIcon(() => map[name] ?? null);
    });
  }, [name]);

  if (!Icon) return null;
  return <Icon {...props} />;
}
