import { Button, Result } from "antd";
import { useNavigate } from "react-router-dom";
import type { Menu } from "@/types";

export default function FallbackPage({ menu }: { menu: Menu.Item }) {
  const navigate = useNavigate();
  return (
    <Result
      status="info"
      title={`「${menu.name}」页面建设中`}
      subTitle={
        <>
          路由地址：<code>{menu.path}</code>
          <br />
          组件配置：<code>{menu.component ?? "未配置"}</code>
        </>
      }
      extra={
        <Button type="primary" onClick={() => navigate("/")}>
          返回首页
        </Button>
      }
    />
  );
}
