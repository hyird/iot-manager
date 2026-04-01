import { HolderOutlined } from "@ant-design/icons";
import type { DragEndEvent } from "@dnd-kit/core";
import { closestCenter, DndContext, PointerSensor, useSensor, useSensors } from "@dnd-kit/core";
import {
  arrayMove,
  SortableContext,
  useSortable,
  verticalListSortingStrategy,
} from "@dnd-kit/sortable";
import { CSS } from "@dnd-kit/utilities";
import { Button, Flex, Space } from "antd";
import type { CSSProperties, ReactNode } from "react";
import { memo, useCallback, useEffect, useMemo, useState } from "react";
import { sortSectionsByOrder } from "./grouping";

interface SortableGroupSectionFrameProps {
  id: string;
  title: ReactNode;
  description?: ReactNode;
  meta?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
  disabled?: boolean;
  className?: string;
  bodyClassName?: string;
  style?: CSSProperties;
}

export const SortableGroupSectionFrame = memo(
  ({
    id,
    title,
    description,
    meta,
    actions,
    children,
    disabled = false,
    className,
    bodyClassName,
    style,
  }: SortableGroupSectionFrameProps) => {
    const { attributes, listeners, setNodeRef, transform, transition, isDragging } = useSortable({
      id,
      disabled,
    });

    return (
      <section
        ref={setNodeRef}
        className={className}
        style={{
          transform: CSS.Transform.toString(transform),
          transition: transition || undefined,
          opacity: isDragging ? 0.75 : 1,
          zIndex: isDragging ? 1 : undefined,
          ...style,
        }}
      >
        <Flex justify="space-between" align="center" gap={12} wrap className="mb-3">
          <div className="min-w-0">
            <div className="text-sm font-semibold text-slate-800">{title}</div>
            {description ? <div className="mt-1 text-xs text-slate-500">{description}</div> : null}
          </div>

          <Space size={6} wrap>
            {meta}
            {actions}
            <Button
              type="text"
              size="small"
              icon={<HolderOutlined />}
              disabled={disabled}
              aria-label="拖拽排序"
              title="拖拽排序"
              className="!cursor-grab active:!cursor-grabbing"
              style={{ touchAction: "none" }}
              {...attributes}
              {...listeners}
            />
          </Space>
        </Flex>

        <div className={bodyClassName}>{children}</div>
      </section>
    );
  }
);

interface SortableGroupSectionListProps<T extends { key: string }> {
  sections: T[];
  children: (section: T) => ReactNode;
  empty?: ReactNode;
  className?: string;
  disabled?: boolean;
  onOrderChange?: (nextOrder: string[]) => void;
}

const getSectionSignature = (sections: { key: string }[]) =>
  JSON.stringify(sections.map((section) => section.key));

const areOrdersEqual = (left: string[], right: string[]) =>
  left.length === right.length && left.every((value, index) => value === right[index]);

const reconcileOrder = (previousOrder: string[], currentKeys: string[]) => {
  if (currentKeys.length === 0) return [];
  if (previousOrder.length === 0) return currentKeys;

  const currentKeySet = new Set(currentKeys);
  const filtered = previousOrder.filter((key) => currentKeySet.has(key));
  const missing = currentKeys.filter((key) => !filtered.includes(key));

  if (filtered.length === 0) return currentKeys;
  const nextOrder = [...filtered, ...missing];
  return areOrdersEqual(previousOrder, nextOrder) ? previousOrder : nextOrder;
};

export const SortableGroupSectionList = <T extends { key: string }>({
  sections,
  children,
  empty,
  className,
  disabled = false,
  onOrderChange,
}: SortableGroupSectionListProps<T>) => {
  const sectionSignature = useMemo(() => getSectionSignature(sections), [sections]);
  const currentKeys = useMemo(() => sections.map((section) => section.key), [sectionSignature]);
  const [order, setOrder] = useState<string[]>(currentKeys);

  useEffect(() => {
    setOrder((previousOrder) => reconcileOrder(previousOrder, currentKeys));
  }, [currentKeys, sectionSignature]);

  const orderedSections = useMemo(() => sortSectionsByOrder(sections, order), [sections, order]);

  const sensors = useSensors(useSensor(PointerSensor, { activationConstraint: { distance: 8 } }));

  const handleDragEnd = useCallback(
    ({ active, over }: DragEndEvent) => {
      if (!over || active.id === over.id) return;

      const activeKey = String(active.id);
      const overKey = String(over.id);
      const activeIndex = order.indexOf(activeKey);
      const overIndex = order.indexOf(overKey);
      if (activeIndex < 0 || overIndex < 0) return;

      const nextOrder = arrayMove(order, activeIndex, overIndex);
      setOrder(nextOrder);
      onOrderChange?.(nextOrder);
    },
    [onOrderChange, order]
  );

  if (orderedSections.length === 0) {
    return empty ?? null;
  }

  return (
    <DndContext
      sensors={sensors}
      collisionDetection={closestCenter}
      onDragEnd={disabled ? undefined : handleDragEnd}
    >
      <SortableContext
        items={orderedSections.map((section) => section.key)}
        strategy={verticalListSortingStrategy}
      >
        <Space direction="vertical" className={className ? className : "w-full"} size="large">
          {orderedSections.map(children)}
        </Space>
      </SortableContext>
    </DndContext>
  );
};
