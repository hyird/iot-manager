import ReactDOM from "react-dom/client";
import { Provider } from "react-redux";
import { PersistGate } from "redux-persist/integration/react";
import { StyleProvider } from "@ant-design/cssinjs";
import { ConfigProvider, App as AntdApp, Spin } from "antd";
import zhCN from "antd/locale/zh_CN";
import { store, persistor } from "./store";
import { TanstackQuery } from "./providers/TanstackQuery";
import { initSentry } from "./utils/sentry";
import { AppRoutes } from "./routes";
import "./styles/index.css";

// 初始化 Sentry
initSentry();

ReactDOM.createRoot(document.getElementById("root")!).render(
  <Provider store={store}>
    <PersistGate loading={<Spin fullscreen />} persistor={persistor}>
      <StyleProvider hashPriority="low" layer>
        <ConfigProvider
          locale={zhCN}
          theme={{
            token: {
              colorPrimary: "#1677ff",
            },
          }}
        >
          <AntdApp>
            <TanstackQuery>
              <AppRoutes />
            </TanstackQuery>
          </AntdApp>
        </ConfigProvider>
      </StyleProvider>
    </PersistGate>
  </Provider>
);
