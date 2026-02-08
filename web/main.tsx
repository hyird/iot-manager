import { StyleProvider } from "@ant-design/cssinjs";
import { App as AntdApp, ConfigProvider, Spin } from "antd";
import zhCN from "antd/locale/zh_CN";
import ReactDOM from "react-dom/client";
import { Provider } from "react-redux";
import { PersistGate } from "redux-persist/integration/react";
import { TanstackQuery } from "./providers/TanstackQuery";
import { WebSocketProvider } from "./providers/WebSocketProvider";
import { AppRoutes } from "./routes";
import { persistor, store } from "./store";
import "./styles/index.css";

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
              <WebSocketProvider>
                <AppRoutes />
              </WebSocketProvider>
            </TanstackQuery>
          </AntdApp>
        </ConfigProvider>
      </StyleProvider>
    </PersistGate>
  </Provider>
);
