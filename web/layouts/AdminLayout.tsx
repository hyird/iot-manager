import {
  LogoutOutlined,
  MenuFoldOutlined,
  MenuOutlined,
  MenuUnfoldOutlined,
  UserOutlined,
} from "@ant-design/icons";
import type { MenuProps } from "antd";
import { Menu as AntdMenu, Button, Drawer, Dropdown, Grid, Layout, Space, Spin } from "antd";
import type { ItemType } from "antd/es/menu/interface";
import { useEffect, useMemo, useState } from "react";
import { useLocation, useNavigate } from "react-router-dom";
import LayoutBreadcrumb from "@/components/LayoutBreadcrumb";
import PageTabs from "@/components/PageTabs";
import { PageTransition } from "@/components/PageTransition";
import { useCurrentUser } from "@/services";
import { useAuthStore, useTabsStore } from "@/store/hooks";
import type { Menu } from "@/types";
import { buildMenuTree } from "@/utils";
import { renderIcon } from "@/utils/icon";

const { Header, Sider, Content } = Layout;
const { useBreakpoint } = Grid;

export default function AdminLayout() {
  const { token, clearAuth } = useAuthStore();
  const { clearTabs } = useTabsStore();
  const { data: user } = useCurrentUser();
  const navigate = useNavigate();
  const location = useLocation();
  const screens = useBreakpoint();

  const [collapsed, setCollapsed] = useState(false);
  const [mobileDrawerOpen, setMobileDrawerOpen] = useState(false);

  // 判断是否为移动端
  const isMobile = !screens.md;

  // 有 token 但完全没有 user 缓存时才显示 loading
  const isInitialLoading = !!token && !user;

  // 路由变化时关闭移动端抽屉
  useEffect(() => {
    if (isMobile && mobileDrawerOpen) {
      setMobileDrawerOpen(false);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [isMobile, mobileDrawerOpen]);

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
        clearAuth();
        clearTabs();
        // AuthGuard 会自动检测到 token 为空并重定向到登录页
      }
    },
  };

  // 侧边栏菜单内容
  const siderContent = (
    <>
      <div className="h-12 m-2 rounded flex items-center justify-center shrink-0 text-white font-medium">
        {!isInitialLoading && <span>{collapsed && !isMobile ? "IoT" : "物联平台"}</span>}
      </div>
      {!isInitialLoading && (
        <div className="overflow-auto h-[calc(100vh-64px)] scrollbar-none">
          <AntdMenu
            theme="dark"
            mode="inline"
            selectedKeys={[location.pathname]}
            items={menuItems}
            onClick={onMenuClick}
            inlineCollapsed={collapsed && !isMobile}
          />
        </div>
      )}
    </>
  );

  return (
    <Layout className="h-screen overflow-hidden">
      {/* 桌面端：固定侧边栏 */}
      {!isMobile && (
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
      )}

      {/* 移动端：抽屉菜单 */}
      {isMobile && (
        <Drawer
          placement="left"
          open={mobileDrawerOpen}
          onClose={() => setMobileDrawerOpen(false)}
          styles={{ body: { padding: 0, background: "#001529" } }}
          size={220}
        >
          {siderContent}
        </Drawer>
      )}

      <Layout className="flex flex-col overflow-hidden bg-[#f5f5f5]">
        <Header className="h-12 px-4 bg-white flex items-center justify-between shrink-0 shadow-[0_2px_8px_rgba(0,0,0,0.06)] relative z-10">
          <div className="flex items-center gap-2 flex-1">
            {/* 移动端：显示菜单按钮 */}
            {isMobile && (
              <Button
                type="text"
                icon={<MenuOutlined />}
                onClick={() => setMobileDrawerOpen(true)}
              />
            )}
            {/* 桌面端：显示折叠按钮 */}
            {!isMobile && (
              <Button
                type="text"
                icon={collapsed ? <MenuUnfoldOutlined /> : <MenuFoldOutlined />}
                onClick={() => setCollapsed(!collapsed)}
              />
            )}
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
                    <span className="hidden sm:inline">{user?.username}</span>
                  </Space>
                </Button>
              </Dropdown>
            )}
          </div>
        </Header>

        {!isInitialLoading && <PageTabs />}

        <Content className="m-2 sm:m-4 bg-white flex flex-col flex-1 overflow-hidden rounded-lg">
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
