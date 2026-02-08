import { CloseOutlined, HomeOutlined } from "@ant-design/icons";
import type { DragEndEvent } from "@dnd-kit/core";
import { closestCenter, DndContext, PointerSensor, useSensor, useSensors } from "@dnd-kit/core";
import { horizontalListSortingStrategy, SortableContext, useSortable } from "@dnd-kit/sortable";
import { CSS } from "@dnd-kit/utilities";
import type { MenuProps } from "antd";
import { Button, Dropdown, Tabs, Tooltip } from "antd";
import type { CSSProperties } from "react";
import React, { useCallback, useEffect, useMemo, useRef } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import { HOME_TAB, useAuthStore, useTabsStore } from "@/store/hooks";
import type { Menu } from "@/types";
import { buildMenuTree } from "@/utils";

/** 可拖拽标签节点 — 用 cloneElement 注入，不额外包裹 div */
function SortableTab({ node }: { node: React.ReactElement }) {
  const nodeKey = (node.props as Record<string, unknown>)["data-node-key"] as string;
  const { attributes, listeners, setNodeRef, transform, transition, isDragging } = useSortable({
    id: nodeKey,
  });

  return React.cloneElement(node as React.ReactElement<Record<string, unknown>>, {
    ref: setNodeRef,
    style: {
      ...((node.props as Record<string, unknown>).style as CSSProperties),
      transform: CSS.Translate.toString(transform),
      transition: transition || undefined,
      opacity: isDragging ? 0.5 : 1,
    },
    ...attributes,
    ...listeners,
  });
}

export default function PageTabs() {
  const navigate = useNavigate();
  const location = useLocation();
  const { user } = useAuthStore();
  const { tabs, activeKey, addTab, removeTab, setActiveKey, clearTabs, setTabsState } =
    useTabsStore();

  const prevPathRef = useRef<string>(location.pathname);

  const menuTree = useMemo<Menu.TreeItem[]>(() => {
    const menus = user?.menus || [];
    return buildMenuTree(menus);
  }, [user?.menus]);

  // Build path -> title map
  const pathTitleMap = useMemo<Map<string, string>>(() => {
    const map = new Map<string, string>();

    const traverse = (items: Menu.TreeItem[]) => {
      for (const item of items) {
        if (item.full_path && item.type === "page") {
          map.set(item.full_path, item.name);
        }
        if (item.children) {
          traverse(item.children);
        }
      }
    };

    traverse(menuTree);
    return map;
  }, [menuTree]);

  // Auto add tab when route changes (except home)
  useEffect(() => {
    const path = location.pathname;

    // 只有路径真正变化时才处理
    if (path === prevPathRef.current) {
      return;
    }
    prevPathRef.current = path;

    if (path === HOME_TAB.key) {
      setActiveKey(HOME_TAB.key);
      return;
    }

    const title = pathTitleMap.get(path);
    if (title) {
      // 检查标签是否已存在
      const exists = tabs.some((t) => t.key === path);
      if (!exists) {
        addTab({ key: path, title });
      } else {
        // 标签已存在，只切换激活状态
        setActiveKey(path);
      }
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [
    location.pathname,
    pathTitleMap,
    addTab, // 标签已存在，只切换激活状态
    setActiveKey,
    tabs,
  ]);

  const handleTabChange = (key: string) => {
    setActiveKey(key);
    navigate(key);
  };

  const handleTabEdit = useCallback(
    (targetKey: React.MouseEvent | React.KeyboardEvent | string, action: "add" | "remove") => {
      if (action === "remove" && typeof targetKey === "string") {
        // Cannot close home tab
        if (targetKey === HOME_TAB.key) return;

        const newActiveKey = removeTab(targetKey);
        if (newActiveKey) {
          navigate(newActiveKey);
        }
      }
    },
    [removeTab, navigate]
  );

  const getContextMenu = useCallback(
    (tabKey: string): MenuProps => {
      const isHome = tabKey === HOME_TAB.key;
      const currentIndex = tabs.findIndex((t) => t.key === tabKey);
      const closableTabs = tabs.filter((t) => t.key !== HOME_TAB.key);

      return {
        items: [
          {
            key: "close",
            label: "关闭",
            disabled: isHome,
          },
          {
            key: "closeOther",
            label: "关闭其他",
            disabled: closableTabs.length <= (isHome ? 0 : 1),
          },
          {
            key: "closeRight",
            label: "关闭右侧",
            disabled: currentIndex >= tabs.length - 1,
          },
          {
            key: "closeAll",
            label: "关闭全部",
            disabled: closableTabs.length === 0,
          },
        ],
        onClick: ({ key }) => {
          if (key === "close") {
            if (!isHome) {
              handleTabEdit(tabKey, "remove");
            }
          } else if (key === "closeOther") {
            const currentTab = tabs.find((t) => t.key === tabKey);
            const newTabs = isHome ? [HOME_TAB] : [HOME_TAB, currentTab!].filter(Boolean);
            setTabsState(newTabs, tabKey);
          } else if (key === "closeRight") {
            const newTabs = tabs.slice(0, currentIndex + 1);
            const newActiveKey = newTabs.some((t) => t.key === activeKey)
              ? activeKey
              : newTabs[newTabs.length - 1]?.key || HOME_TAB.key;
            setTabsState(newTabs, newActiveKey);
            if (newActiveKey !== activeKey) navigate(newActiveKey);
          } else if (key === "closeAll") {
            clearTabs();
            navigate(HOME_TAB.key);
          }
        },
      };
    },
    [tabs, activeKey, clearTabs, navigate, setTabsState, handleTabEdit]
  );

  const hasClosableTabs = tabs.some((t) => t.key !== HOME_TAB.key);

  const handleCloseAll = useCallback(() => {
    clearTabs();
    navigate(HOME_TAB.key);
  }, [clearTabs, navigate]);

  const tabItems = useMemo(
    () =>
      tabs.map((tab) => ({
        key: tab.key,
        label: (
          <Dropdown menu={getContextMenu(tab.key)} trigger={["contextMenu"]}>
            <span className="inline-flex items-center gap-1">
              {tab.key === HOME_TAB.key && <HomeOutlined />}
              {tab.title}
            </span>
          </Dropdown>
        ),
        closable: tab.key !== HOME_TAB.key,
      })),
    [tabs, getContextMenu]
  );

  // DnD 拖拽排序
  const sensors = useSensors(useSensor(PointerSensor, { activationConstraint: { distance: 10 } }));

  const handleDragEnd = useCallback(
    ({ active, over }: DragEndEvent) => {
      if (!over || active.id === over.id) return;
      const activeIndex = tabs.findIndex((t) => t.key === active.id);
      const overIndex = tabs.findIndex((t) => t.key === over.id);
      if (activeIndex < 0 || overIndex < 0) return;
      const newTabs = [...tabs];
      const [removed] = newTabs.splice(activeIndex, 1);
      newTabs.splice(overIndex, 0, removed);
      setTabsState(newTabs, activeKey);
    },
    [tabs, activeKey, setTabsState]
  );

  return (
    <DndContext sensors={sensors} collisionDetection={closestCenter} onDragEnd={handleDragEnd}>
      <SortableContext items={tabs.map((t) => t.key)} strategy={horizontalListSortingStrategy}>
        <Tabs
          type="editable-card"
          hideAdd
          activeKey={activeKey}
          onChange={handleTabChange}
          onEdit={handleTabEdit}
          items={tabItems}
          size="small"
          className="page-tabs px-4 pt-2 pb-0 bg-white shrink-0"
          renderTabBar={(tabBarProps, DefaultTabBar) => (
            <DefaultTabBar {...tabBarProps}>
              {(node) => <SortableTab key={node.key} node={node} />}
            </DefaultTabBar>
          )}
          tabBarExtraContent={
            hasClosableTabs && (
              <Tooltip title="关闭所有标签页">
                <Button
                  type="text"
                  size="small"
                  icon={<CloseOutlined />}
                  onClick={handleCloseAll}
                  className="mr-2 text-gray-400 hover:text-gray-600"
                />
              </Tooltip>
            )
          }
        />
      </SortableContext>
    </DndContext>
  );
}
