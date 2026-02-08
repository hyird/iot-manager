import {
  LogoutOutlined,
  MenuFoldOutlined,
  MenuUnfoldOutlined,
  SearchOutlined,
  UserOutlined,
} from "@ant-design/icons";
import type { MenuProps } from "antd";
import { Menu as AntdMenu, Button, Dropdown, Input, Layout, Space, Spin } from "antd";
import type { ItemType } from "antd/es/menu/interface";
import { useMemo, useState } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import LayoutBreadcrumb from "@/components/LayoutBreadcrumb";
import PageTabs from "@/components/PageTabs";
import { PageTransition } from "@/components/PageTransition";
import { useLogout } from "@/services/auth";
import { useCurrentUser } from "@/services";
import { useAuthStore, useTabsStore } from "@/store/hooks";
import type { Menu } from "@/types";
import { buildMenuTree } from "@/utils";
import { renderIcon } from "@/utils/icon";

const { Header, Sider, Content } = Layout;

export default function AdminLayout() {
  const { token } = useAuthStore();
  const { clearTabs } = useTabsStore();
  const { data: user } = useCurrentUser();
  const logoutMutation = useLogout();
  const navigate = useNavigate();
  const location = useLocation();

  const [collapsed, setCollapsed] = useState(false);
  const [menuSearch, setMenuSearch] = useState("");

  // 有 token 但完全没有 user 缓存时才显示 loading
  const isInitialLoading = !!token && !user;

  const menuTree = useMemo<Menu.TreeItem[]>(() => {
    const menus = user?.menus || [];
    return buildMenuTree(menus);
  }, [user?.menus]);

  const menuItems = useMemo(() => {
    const buildMenuItems = (items: Menu.TreeItem[]): ItemType[] => {
      return items
        .filter((m) => m.type === "menu" || m.type === "page")
        .map((m) => {
          const children = m.children?.filter((c) => c.type === "menu" || c.type === "page");
          if (children && children.length > 0) {
            return {
              key: m.full_path || m.path || `menu-${m.id}`,
              icon: renderIcon(m.icon),
              label: m.name,
              children: buildMenuItems(children),
            };
          }
          // type="menu" 但没有子项的菜单，使用特殊 key 标记
          if (m.type === "menu") {
            return {
              key: `menu-${m.id}`,
              icon: renderIcon(m.icon),
              label: m.name,
            };
          }
          return {
            key: m.full_path || m.path || `menu-${m.id}`,
            icon: renderIcon(m.icon),
            label: m.name,
          };
        });
    };
    return buildMenuItems(menuTree);
  }, [menuTree]);

  // 菜单搜索过滤
  const filteredMenuItems = useMemo(() => {
    if (!menuSearch.trim()) return menuItems;
    const keyword = menuSearch.trim().toLowerCase();
    const filterItems = (items: ItemType[]): ItemType[] => {
      return items
        .map((item) => {
          if (!item || !("key" in item)) return null;
          const menuItem = item as { key: string; label?: string; children?: ItemType[] };
          if (menuItem.children?.length) {
            const filtered = filterItems(menuItem.children);
            if (filtered.length > 0) return { ...menuItem, children: filtered };
          }
          const label = typeof menuItem.label === "string" ? menuItem.label : "";
          if (label.toLowerCase().includes(keyword)) return item;
          return null;
        })
        .filter(Boolean) as ItemType[];
    };
    return filterItems(menuItems);
  }, [menuItems, menuSearch]);

  const onMenuClick = (info: { key: string }) => {
    // 空目录菜单（key 以 menu- 开头）点击无反应
    if (info.key.startsWith("menu-")) return;
    navigate(info.key);
  };

  const userMenu: MenuProps = {
    items: [
      {
        key: "logout",
        icon: <LogoutOutlined />,
        label: "退出登录",
      },
    ],
    onClick: (info) => {
      if (info.key === "logout") {
        clearTabs();
        logoutMutation.mutate();
      }
    },
  };

  // 侧边栏菜单内容
  const siderContent = (
    <>
      <div className="h-12 m-2 rounded flex items-center justify-center shrink-0 text-white font-medium">
        {!isInitialLoading && <span>{collapsed ? "IoT" : "物联平台"}</span>}
      </div>
      {!isInitialLoading && (
        <>
          {!collapsed && (
            <div className="px-3 mb-2">
              <Input
                allowClear
                prefix={<SearchOutlined className="!text-white/30" />}
                placeholder="搜索菜单"
                variant="borderless"
                value={menuSearch}
                onChange={(e) => setMenuSearch(e.target.value)}
                className="sider-search"
              />
            </div>
          )}
          <div className="overflow-auto flex-1 scrollbar-none">
            <AntdMenu
              theme="dark"
              mode="inline"
              selectedKeys={[location.pathname]}
              items={filteredMenuItems}
              onClick={onMenuClick}
              inlineCollapsed={collapsed}
            />
          </div>
        </>
      )}
    </>
  );

  return (
    <Layout className="h-screen overflow-hidden">
      <Sider
        width={220}
        className="flex flex-col"
        collapsible
        collapsed={collapsed}
        onCollapse={setCollapsed}
        trigger={null}
      >
        {siderContent}
      </Sider>

      <Layout className="flex flex-col overflow-hidden bg-[#f5f5f5]">
        <Header className="h-12 px-4 bg-white flex items-center justify-between shrink-0 shadow-[0_2px_8px_rgba(0,0,0,0.06)] relative z-10">
          <div className="flex items-center gap-2 flex-1">
            <Button
              type="text"
              icon={collapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />}
              onClick={() => setCollapsed(!collapsed)}
            />
            {!isInitialLoading && <LayoutBreadcrumb />}
          </div>
          <div className="flex justify-end items-center gap-3">
            {isInitialLoading ? (
              <div className="flex items-center gap-2">
                <UserOutlined />
              </div>
            ) : (
              <Dropdown menu={userMenu} placement="bottomRight" trigger={["click"]}>
                <Button>
                  <Space>
                    <UserOutlined />
                    <span>{user?.username}</span>
                  </Space>
                </Button>
              </Dropdown>
            )}
          </div>
        </Header>

        {!isInitialLoading && <PageTabs />}

        <Content className="m-4 bg-white flex flex-col flex-1 overflow-hidden rounded-lg">
          {isInitialLoading ? (
            <div className="flex-1 flex items-center justify-center">
              <Spin tip="加载中..." fullscreen />
            </div>
          ) : (
            <div className="flex-1 flex flex-col overflow-auto">
              <PageTransition />
            </div>
          )}
        </Content>
      </Layout>
    </Layout>
  );
}
