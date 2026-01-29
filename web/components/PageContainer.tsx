import type { ReactNode } from "react";

interface PageContainerProps {
  /** 页面标题（可选，暂未使用，保留接口） */
  title?: string;
  /** 页面标题区域（搜索栏、操作按钮等） - 固定不滚动 */
  header?: ReactNode;
  /** 页面主体内容 - 可滚动 */
  children: ReactNode;
}

/**
 * 页面容器组件
 * - header: 固定在顶部的搜索/操作栏
 * - children: 可滚动的主体内容
 */
export function PageContainer({ header, children }: PageContainerProps) {
  return (
    <div className="h-full flex flex-col overflow-hidden p-4">
      {header && <div className="shrink-0 pb-4">{header}</div>}
      <div className="flex-1 overflow-y-auto overflow-x-hidden">{children}</div>
    </div>
  );
}
