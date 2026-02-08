import { DownOutlined, HomeOutlined } from "@ant-design/icons";
import type { MenuProps } from "antd";
import { Breadcrumb, Dropdown } from "antd";
import { useMemo } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import DynamicIcon from "@/components/DynamicIcon";
import { useAuthStore } from "@/store/hooks";
import type { Menu } from "@/types";
import { buildMenuTree } from "@/utils";

interface BreadcrumbItemData {
  item: Menu.TreeItem;
  isLast: boolean;
}

export default function LayoutBreadcrumb() {
  const location = useLocation();
  const navigate = useNavigate();
  const { user } = useAuthStore();

  const menuTree = useMemo<Menu.TreeItem[]>(() => {
    const menus = user?.menus || [];
    return buildMenuTree(menus);
  }, [user?.menus]);

  // Build a flat map of path -> menu item with parent chain
  const pathMap = useMemo(() => {
    const map = new Map<string, Menu.TreeItem[]>();

    const traverse = (items: Menu.TreeItem[], parents: Menu.TreeItem[] = []) => {
      for (const item of items) {
        if (item.full_path) {
          map.set(item.full_path, [...parents, item]);
        }
        if (item.children) {
          traverse(item.children, [...parents, item]);
        }
      }
    };

    traverse(menuTree);
    return map;
  }, [menuTree]);

  const breadcrumbData = useMemo<BreadcrumbItemData[]>(() => {
    const chain = pathMap.get(location.pathname);
    if (!chain || chain.length === 0) {
      return [];
    }

    return chain.map((item, index) => ({
      item,
      isLast: index === chain.length - 1,
    }));
  }, [location.pathname, pathMap]);

  // 首页显示默认面包屑
  if (location.pathname === "/home") {
    return (
      <Breadcrumb
        items={[
          {
            title: (
              <span className="inline-flex items-center gap-1">
                <HomeOutlined />
                首页
              </span>
            ),
          },
        ]}
      />
    );
  }

  if (breadcrumbData.length === 0) {
    return null;
  }

  const renderBreadcrumbItem = (data: BreadcrumbItemData) => {
    const { item, isLast } = data;

    // Get children that are pages (can be navigated to)
    const pageChildren =
      item.children?.filter((child) => child.type === "page" && child.full_path) || [];

    // If this menu has page children, show dropdown
    if (pageChildren.length > 0) {
      const menuItems: MenuProps["items"] = pageChildren.map((child) => ({
        key: child.id,
        icon: child.icon ? <DynamicIcon name={child.icon} /> : null,
        label: child.name,
        disabled: location.pathname === child.full_path,
        onClick: () => {
          if (child.full_path && location.pathname !== child.full_path) {
            navigate(child.full_path);
          }
        },
      }));

      return (
        <Dropdown menu={{ items: menuItems }} trigger={["hover"]}>
          <span className="cursor-pointer hover:text-blue-500 inline-flex items-center gap-1">
            {item.icon && <DynamicIcon name={item.icon} />}
            {item.name}
            <DownOutlined className="text-[10px]" />
          </span>
        </Dropdown>
      );
    }

    // Last item or no children - show as text with icon
    if (isLast) {
      return (
        <span className="inline-flex items-center gap-1">
          {item.icon && <DynamicIcon name={item.icon} />}
          {item.name}
        </span>
      );
    }

    // Has link - show as clickable
    if (item.full_path && item.type === "page") {
      return (
        <span
          className="cursor-pointer hover:text-blue-500 inline-flex items-center gap-1"
          onClick={() => navigate(item.full_path!)}
        >
          {item.icon && <DynamicIcon name={item.icon} />}
          {item.name}
        </span>
      );
    }

    return (
      <span className="inline-flex items-center gap-1">
        {item.icon && <DynamicIcon name={item.icon} />}
        {item.name}
      </span>
    );
  };

  return (
    <Breadcrumb
      items={breadcrumbData.map((data) => ({
        title: renderBreadcrumbItem(data),
      }))}
    />
  );
}
