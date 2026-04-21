import { StyleProvider } from "@ant-design/cssinjs";
import { App as AntdApp, ConfigProvider } from "antd";
import { useEffect } from "react";
import zhCN from "antd/locale/zh_CN";
import ReactDOM from "react-dom/client";
import { Provider } from "react-redux";
import { PersistGate } from "redux-persist/integration/react";
import { ErrorBoundary } from "./components/ErrorBoundary";
import { AntdMessageHolder } from "./providers/AntdMessageHolder";
import { TanstackQuery } from "./providers/TanstackQuery";
import { WebSocketProvider } from "./providers/WebSocketProvider";
import { AppRoutes } from "./routes";
import { persistor, store } from "./store";
import "./styles/index.css";

function hideInitialLoading() {
  const loadingElement = document.getElementById("app-loading");
  if (!loadingElement || loadingElement.classList.contains("app-loading--hide")) {
    return;
  }

  const removeLoading = () => {
    loadingElement.remove();
  };

  loadingElement.classList.add("app-loading--hide");
  loadingElement.addEventListener("animationend", removeLoading, { once: true });

  // 动画异常兜底，避免遮罩残留
  window.setTimeout(removeLoading, 1200);
}

function AppReadyMarker() {
  useEffect(() => {
    hideInitialLoading();
  }, []);

  return null;
}

const rootElement = document.getElementById("root");
if (!rootElement) throw new Error("Root element #root not found in DOM");

ReactDOM.createRoot(rootElement).render(
  <Provider store={store}>
    <PersistGate loading={null} persistor={persistor}>
      <AppReadyMarker />
      <ErrorBoundary>
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
              <AntdMessageHolder />
              <TanstackQuery>
                <WebSocketProvider>
                  <AppRoutes />
                </WebSocketProvider>
              </TanstackQuery>
            </AntdApp>
          </ConfigProvider>
        </StyleProvider>
      </ErrorBoundary>
    </PersistGate>
  </Provider>
);
