import { message } from "antd";
import { useEffect } from "react";

interface PageLoadingProps {
  tip?: string;
}
export function PageLoading({ tip = "页面加载中..." }: PageLoadingProps) {
  const [messageApi, contextHolder] = message.useMessage();

  useEffect(() => {
    const key = "page-loading";
    messageApi.open({
      key,
      type: "loading",
      content: tip,
      duration: 0,
    });
    return () => {
      messageApi.destroy(key);
    };
  }, [messageApi, tip]);
  return <>{contextHolder}</>;
}
