#pragma once

/**
 * @brief L2 配置工具 GUI - 基于 ImGui + DirectX11
 *
 * 功能:
 * - 自动枚举网卡
 * - 自动发现设备（定时广播）
 * - 一键建立 SSH 隧道
 * - 显示隧道状态
 */

#include "L2TcpTunnel.hpp"
#include "L2Transport.hpp"
#include "common/l2config/L2Protocol.hpp"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

#include <d3d11.h>
#include <tchar.h>

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace l2config_tool {

class L2ConfigApp {
public:
    L2ConfigApp() = default;
    ~L2ConfigApp() { cleanup(); }

    int run() {
        if (!createWindow()) return 1;
        if (!createDevice()) { cleanupWindow(); return 1; }
        if (!initImGui()) { cleanupDevice(); cleanupWindow(); return 1; }

        // 枚举网卡
        refreshNics();

        // 主循环
        MSG msg{};
        while (running_) {
            while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                ::TranslateMessage(&msg);
                ::DispatchMessage(&msg);
                if (msg.message == WM_QUIT) {
                    running_ = false;
                }
            }
            if (!running_) break;

            // 自动扫描设备
            autoScan();

            // 渲染帧
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            renderUI();

            ImGui::Render();
            const float clearColor[] = {0.94f, 0.94f, 0.94f, 1.0f};
            deviceContext_->OMSetRenderTargets(1, &renderTargetView_, nullptr);
            deviceContext_->ClearRenderTargetView(renderTargetView_, clearColor);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            swapChain_->Present(1, 0);  // VSync
        }

        stopTunnel();
        stopTransport();
        cleanup();
        return 0;
    }

private:
    // ==================== 数据结构 ====================

    struct DiscoveredDevice {
        l2config::MacAddr mac{};
        std::string macStr;
        std::string code;
        std::string name;
        std::string version;
        std::vector<std::pair<std::string, std::string>> interfaces;  // name, "ip/prefix [UP/DOWN]"
        std::chrono::steady_clock::time_point lastSeen;
    };

    enum class TunnelState { IDLE, CONNECTING, CONNECTED, FAILED };

    // ==================== UI 渲染 ====================

    void renderUI() {
        auto& io = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);

        ImGui::Begin("L2 Config Tool", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::TextColored({0.09f, 0.47f, 1.0f, 1.0f}, "L2 Configuration Tool");
        ImGui::SameLine();
        ImGui::TextDisabled("Layer 2 SSH Tunnel");
        ImGui::Separator();

        renderNicSelector();
        ImGui::Spacing();
        renderDeviceList();
        ImGui::Spacing();
        renderTunnelStatus();
        ImGui::Spacing();
        renderStatusBar();

        ImGui::End();
    }

    void renderNicSelector() {
        ImGui::Text("Network Interface:");
        ImGui::SameLine();

        // 网卡下拉框
        std::string previewName = selectedNic_ < nics_.size()
            ? nics_[selectedNic_].displayName + "  (" + nics_[selectedNic_].ip + ")"
            : "Select...";

        ImGui::SetNextItemWidth(400);
        if (ImGui::BeginCombo("##nic", previewName.c_str())) {
            for (size_t i = 0; i < nics_.size(); ++i) {
                const auto& nic = nics_[i];
                std::string label = nic.displayName + "  " +
                    l2config::macToString(nic.mac) + "  " +
                    (nic.ip.empty() ? "(no IP)" : nic.ip) +
                    (nic.up ? "" : "  [DOWN]");

                bool selected = (i == selectedNic_);
                if (ImGui::Selectable(label.c_str(), selected)) {
                    if (i != selectedNic_) {
                        selectedNic_ = i;
                        onNicChanged();
                    }
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - lastRefreshClickTime_).count();
            bool flash = elapsed < 1.0;
            if (flash) ImGui::PushStyleColor(ImGuiCol_Button, {0.2f, 0.7f, 0.3f, 1.0f});
            if (ImGui::Button(flash ? "Refreshed!" : "Refresh NICs")) {
                refreshNics();
                lastRefreshClickTime_ = std::chrono::steady_clock::now();
            }
            if (flash) ImGui::PopStyleColor();
        }

        ImGui::SameLine();
        {
            auto elapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - lastScanClickTime_).count();
            bool flash = elapsed < 1.5;
            if (flash) ImGui::PushStyleColor(ImGuiCol_Button, {0.09f, 0.47f, 1.0f, 1.0f});
            if (ImGui::Button(flash ? "Scanning..." : "Scan Now")) {
                sendDiscovery();
                lastScanClickTime_ = std::chrono::steady_clock::now();
            }
            if (flash) ImGui::PopStyleColor();
        }
    }

    void renderDeviceList() {
        ImGui::Text("Discovered Devices:");

        std::lock_guard<std::mutex> lock(devicesMutex_);

        if (devices_.empty()) {
            ImGui::TextDisabled("  No devices found. Make sure the ARM device is connected to the same network segment.");
            return;
        }

        auto now = std::chrono::steady_clock::now();

        for (size_t i = 0; i < devices_.size(); ++i) {
            const auto& dev = devices_[i];
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - dev.lastSeen).count();

            ImGui::PushID(static_cast<int>(i));

            // 设备卡片
            ImVec4 borderColor = elapsed < 10 ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                              : ImVec4(0.6f, 0.6f, 0.6f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
            ImGui::BeginChild(("dev" + std::to_string(i)).c_str(),
                             {0, 0}, ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);

            // 第一行：MAC + 名称
            ImGui::TextColored({0.09f, 0.47f, 1.0f, 1.0f}, "%s", dev.macStr.c_str());
            ImGui::SameLine();
            if (!dev.name.empty()) {
                ImGui::Text("  %s", dev.name.c_str());
            }
            if (!dev.code.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", dev.code.c_str());
            }
            if (!dev.version.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("v%s", dev.version.c_str());
            }

            // 网络接口信息
            for (const auto& [ifName, ifInfo] : dev.interfaces) {
                ImGui::Text("    %s: %s", ifName.c_str(), ifInfo.c_str());
            }

            // 最后发现时间
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120);
            ImGui::TextDisabled("%llds ago", elapsed);

            // 操作按钮
            bool isTunnelTarget = (tunnelState_ != TunnelState::IDLE && tunnelTargetMac_ == dev.macStr);

            if (isTunnelTarget && tunnelState_ == TunnelState::CONNECTED) {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.8f, 0.2f, 0.2f, 1.0f});
                if (ImGui::Button("Disconnect")) {
                    stopTunnel();
                }
                ImGui::PopStyleColor();
            } else if (isTunnelTarget && tunnelState_ == TunnelState::CONNECTING) {
                ImGui::BeginDisabled();
                ImGui::Button("Connecting...");
                ImGui::EndDisabled();
            } else if (tunnelState_ == TunnelState::IDLE || !isTunnelTarget) {
                if (ImGui::Button("SSH Tunnel")) {
                    startTunnel(dev);
                }
            }

            ImGui::SameLine();
            // 复制 MAC 按钮
            if (ImGui::SmallButton("Copy MAC")) {
                ImGui::SetClipboardText(dev.macStr.c_str());
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }

    void renderTunnelStatus() {
        if (tunnelState_ == TunnelState::IDLE) return;

        ImGui::Separator();

        if (tunnelState_ == TunnelState::CONNECTED) {
            ImGui::TextColored({0.2f, 0.8f, 0.2f, 1.0f}, "SSH Tunnel Active");
            ImGui::Text("  Target: %s", tunnelTargetMac_.c_str());
            ImGui::Text("  Local:  ssh root@localhost -p %d", tunnelLocalPort_);

            // 复制 SSH 命令按钮
            if (ImGui::Button("Copy SSH Command")) {
                std::string cmd = "ssh root@localhost -p " + std::to_string(tunnelLocalPort_);
                ImGui::SetClipboardText(cmd.c_str());
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.8f, 0.2f, 0.2f, 1.0f});
            if (ImGui::Button("Disconnect")) {
                stopTunnel();
            }
            ImGui::PopStyleColor();
        } else if (tunnelState_ == TunnelState::CONNECTING) {
            ImGui::TextColored({1.0f, 0.8f, 0.0f, 1.0f}, "Connecting to %s ...", tunnelTargetMac_.c_str());
        } else if (tunnelState_ == TunnelState::FAILED) {
            ImGui::TextColored({0.8f, 0.2f, 0.2f, 1.0f}, "Tunnel Failed");
            ImGui::Text("  %s", tunnelError_.c_str());
            if (ImGui::Button("Dismiss")) {
                tunnelState_ = TunnelState::IDLE;
            }
        }
    }

    void renderStatusBar() {
        ImGui::Separator();
        if (transportRunning_) {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            ImGui::Text("Status: Listening...  %zu device(s) found", devices_.size());
        } else {
            ImGui::TextDisabled("Status: Transport not started. Select a NIC.");
        }
    }

    // ==================== 业务逻辑 ====================

    void refreshNics() {
        stopTransport();
        nics_ = L2Transport::listInterfaces();

        std::cout << "[L2Config] found " << nics_.size() << " NIC(s)" << std::endl;
        for (size_t i = 0; i < nics_.size(); ++i) {
            std::cout << "  [" << i << "] " << nics_[i].displayName
                      << "  MAC=" << l2config::macToString(nics_[i].mac)
                      << "  IP=" << (nics_[i].ip.empty() ? "(none)" : nics_[i].ip)
                      << (nics_[i].up ? "  UP" : "  DOWN") << std::endl;
        }

        if (nics_.empty()) return;

        // 默认选第一个 UP 的
        selectedNic_ = 0;
        for (size_t i = 0; i < nics_.size(); ++i) {
            if (nics_[i].up) { selectedNic_ = i; break; }
        }
        onNicChanged();
    }

    void onNicChanged() {
        stopTunnel();
        stopTransport();
        {
            std::lock_guard<std::mutex> lock(devicesMutex_);
            devices_.clear();
        }

        if (selectedNic_ >= nics_.size()) return;

        const auto& nic = nics_[selectedNic_];
        std::cout << "[L2Config] opening NIC: " << nic.displayName
                  << " (" << nic.pcapName << ")" << std::endl;

        transport_ = std::make_unique<L2Transport>();
        transport_->setFrameCallback([this](const uint8_t* data, size_t len) {
            onL2Frame(data, len);
        });

        auto error = transport_->start(nic.pcapName, nic.mac);
        if (!error.empty()) {
            std::cerr << "[L2Config] failed to start transport: " << error << std::endl;
            transportRunning_ = false;
            return;
        }
        transportRunning_ = true;
        std::cout << "[L2Config] transport started, listening for device broadcasts..." << std::endl;
    }

    void sendDiscovery() {
        if (!transport_ || !transportRunning_) {
            std::cout << "[L2Config] sendDiscovery skipped: transport not running" << std::endl;
            return;
        }
        if (selectedNic_ >= nics_.size()) return;

        auto frames = l2config::buildFrames(
            l2config::BROADCAST_MAC, nics_[selectedNic_].mac,
            l2config::MsgType::DISCOVER_REQ, 0);
        transport_->sendFrames(frames);
        std::cout << "[L2Config] discovery broadcast sent on " << nics_[selectedNic_].displayName << std::endl;
    }

    void autoScan() {
        // ARM 端主动广播 DISCOVER_RSP，PC 端被动监听
        // 这里仅定期清理超时设备（15 秒未见即移除，ARM 端广播间隔 3 秒）
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - lastScanTime_).count();
        if (elapsed >= 5.0) {
            lastScanTime_ = now;

            std::lock_guard<std::mutex> lock(devicesMutex_);
            devices_.erase(
                std::remove_if(devices_.begin(), devices_.end(),
                    [&now](const DiscoveredDevice& d) {
                        return std::chrono::duration<double>(now - d.lastSeen).count() > 15.0;
                    }),
                devices_.end());
        }
    }

    void onL2Frame(const uint8_t* data, size_t len) {
        auto frameOpt = l2config::parseFrame(data, len);
        if (!frameOpt) return;
        const auto& frame = *frameOpt;

        if (transport_ && frame.srcMac == transport_->localMac()) return;

        auto msgType = static_cast<l2config::MsgType>(frame.header.msgType);

        // 处理设备发现响应（支持分片重组）
        if (msgType == l2config::MsgType::DISCOVER_RSP) {
            auto assembled = discoveryAssembler_.feed(frame);
            if (assembled) {
                std::string payload(assembled->begin(), assembled->end());
                std::cout << "[L2Config] DISCOVER_RSP from " << l2config::macToString(frame.srcMac)
                          << " (" << payload.size() << " bytes)" << std::endl;
                handleDiscoverResponse(frame.srcMac, payload);
            }
        }

        // 转发帧给隧道（隧道不再自行设置回调，避免覆盖）
        if (tunnel_) {
            tunnel_->handleFrame(data, len);
        }
    }

    void handleDiscoverResponse(const l2config::MacAddr& mac, const std::string& jsonStr) {
        DiscoveredDevice dev;
        dev.mac = mac;
        dev.macStr = l2config::macToString(mac);
        dev.lastSeen = std::chrono::steady_clock::now();

        // 解析 JSON
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::istringstream iss(jsonStr);
        std::string errs;
        if (Json::parseFromStream(reader, iss, &root, &errs)) {
            dev.code = root.get("code", "").asString();
            dev.name = root.get("name", "").asString();
            dev.version = root.get("version", "").asString();

            if (root.isMember("interfaces") && root["interfaces"].isArray()) {
                for (const auto& iface : root["interfaces"]) {
                    std::string ifName = iface.get("name", "?").asString();
                    std::string ip = iface.get("ip", "").asString();
                    int prefix = iface.get("prefix_length", 0).asInt();
                    bool up = iface.get("up", false).asBool();

                    std::string info;
                    if (!ip.empty()) {
                        info = ip + "/" + std::to_string(prefix);
                    } else {
                        info = "(no IP)";
                    }
                    info += up ? " [UP]" : " [DOWN]";
                    dev.interfaces.emplace_back(ifName, info);
                }
            }
        }

        std::lock_guard<std::mutex> lock(devicesMutex_);
        // 更新已有设备或添加新设备
        for (auto& d : devices_) {
            if (d.mac == mac) {
                d = dev;
                return;
            }
        }
        devices_.push_back(dev);
    }

    void startTunnel(const DiscoveredDevice& dev) {
        stopTunnel();

        tunnelTargetMac_ = dev.macStr;
        tunnelState_ = TunnelState::CONNECTING;
        tunnelLocalPort_ = 2222;

        // 在后台线程建立隧道
        tunnelThread_ = std::thread([this, mac = dev.mac]() {
            tunnel_ = std::make_unique<L2TcpTunnel>(*transport_, mac);
            auto error = tunnel_->start(static_cast<uint16_t>(tunnelLocalPort_), 22, false);
            if (!error.empty()) {
                tunnelError_ = error;
                tunnelState_ = TunnelState::FAILED;
                tunnel_.reset();
            } else {
                tunnelState_ = TunnelState::CONNECTED;
            }
        });
        tunnelThread_.detach();
    }

    void stopTunnel() {
        tunnelState_ = TunnelState::IDLE;
        tunnelTargetMac_.clear();
        if (tunnel_) {
            tunnel_->stop();
            tunnel_.reset();
        }
    }

    void stopTransport() {
        transportRunning_ = false;
        if (transport_) {
            transport_->stop();
            transport_.reset();
        }
    }

    // ==================== Win32 窗口 ====================

    bool createWindow() {
        // 设置 Per-Monitor DPI 感知（解决高 DPI 下显示模糊）
        auto user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32) {
            using SetDpiAwarenessFn = BOOL(WINAPI*)(void*);
            auto setDpiAwareness = reinterpret_cast<SetDpiAwarenessFn>(
                ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (setDpiAwareness) {
                setDpiAwareness(reinterpret_cast<void*>(-4));  // PER_MONITOR_AWARE_V2
            }
        }

        // 获取主监视器 DPI
        HDC hdc = ::GetDC(nullptr);
        dpiScale_ = static_cast<float>(::GetDeviceCaps(hdc, LOGPIXELSX)) / 96.0f;
        ::ReleaseDC(nullptr, hdc);

        int windowW = static_cast<int>(720 * dpiScale_);
        int windowH = static_cast<int>(560 * dpiScale_);

        wc_ = {sizeof(WNDCLASSEX), CS_CLASSDC, wndProc, 0L, 0L,
               GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
               _T("L2ConfigApp"), nullptr};
        ::RegisterClassEx(&wc_);

        hwnd_ = ::CreateWindow(wc_.lpszClassName, _T("L2 Config Tool"),
                               WS_OVERLAPPEDWINDOW,
                               100, 100, windowW, windowH,
                               nullptr, nullptr, wc_.hInstance, this);
        if (!hwnd_) return false;

        ::ShowWindow(hwnd_, SW_SHOWDEFAULT);
        ::UpdateWindow(hwnd_);
        return true;
    }

    void cleanupWindow() {
        ::DestroyWindow(hwnd_);
        ::UnregisterClass(wc_.lpszClassName, wc_.hInstance);
    }

    static LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
            return true;

        auto* app = reinterpret_cast<L2ConfigApp*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));

        switch (msg) {
            case WM_CREATE: {
                auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
                ::SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
                return 0;
            }
            case WM_SIZE:
                if (wParam == SIZE_MINIMIZED) return 0;
                if (app) {
                    app->resizeWidth_ = LOWORD(lParam);
                    app->resizeHeight_ = HIWORD(lParam);
                    app->resizeBuffers();
                }
                return 0;
            case WM_DPICHANGED:
                if (app) {
                    app->dpiScale_ = static_cast<float>(HIWORD(wParam)) / 96.0f;
                    auto* rect = reinterpret_cast<RECT*>(lParam);
                    ::SetWindowPos(hWnd, nullptr, rect->left, rect->top,
                                   rect->right - rect->left, rect->bottom - rect->top,
                                   SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return 0;
        }
        return ::DefWindowProc(hWnd, msg, wParam, lParam);
    }

    // ==================== DirectX 11 ====================

    bool createDevice() {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate = {60, 1};
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd_;
        sd.SampleDesc = {1, 0};
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL featureLevel;
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION,
            &sd, &swapChain_, &device_, &featureLevel, &deviceContext_);

        if (FAILED(hr)) return false;

        createRenderTarget();
        return true;
    }

    void createRenderTarget() {
        ID3D11Texture2D* backBuffer = nullptr;
        swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (backBuffer) {
            device_->CreateRenderTargetView(backBuffer, nullptr, &renderTargetView_);
            backBuffer->Release();
        }
    }

    void cleanupRenderTarget() {
        if (renderTargetView_) { renderTargetView_->Release(); renderTargetView_ = nullptr; }
    }

    void resizeBuffers() {
        if (!device_) return;
        cleanupRenderTarget();
        swapChain_->ResizeBuffers(0, resizeWidth_, resizeHeight_,
                                  DXGI_FORMAT_UNKNOWN, 0);
        createRenderTarget();
    }

    void cleanupDevice() {
        cleanupRenderTarget();
        if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
        if (deviceContext_) { deviceContext_->Release(); deviceContext_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
    }

    // ==================== ImGui ====================

    bool initImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;  // 不保存窗口布局

        ImGui::StyleColorsLight();
        auto& style = ImGui::GetStyle();
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.WindowRounding = 0.0f;

        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_, deviceContext_);

        // 加载中文字体
        loadChineseFont(io);

        return true;
    }

    void loadChineseFont(ImGuiIO& io) {
        float fontSize = 16.0f * dpiScale_;

        // 尝试加载微软雅黑，支持中文显示
        const char* fontPaths[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",    // 微软雅黑
            "C:\\Windows\\Fonts\\simhei.ttf",   // 黑体
            "C:\\Windows\\Fonts\\simsun.ttc",   // 宋体
        };

        for (const auto* path : fontPaths) {
            if (std::filesystem::exists(path)) {
                io.Fonts->AddFontFromFileTTF(path, fontSize, nullptr,
                                              io.Fonts->GetGlyphRangesChineseFull());
                return;
            }
        }

        // 回退到默认字体
        io.Fonts->AddFontDefault();
    }

    void cleanupImGui() {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    void cleanup() {
        cleanupImGui();
        cleanupDevice();
        cleanupWindow();
    }

    // ==================== 数据成员 ====================

    bool running_ = true;
    float dpiScale_ = 1.0f;

    // Win32
    HWND hwnd_ = nullptr;
    WNDCLASSEX wc_{};
    UINT resizeWidth_ = 0;
    UINT resizeHeight_ = 0;

    // DirectX 11
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* deviceContext_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* renderTargetView_ = nullptr;

    // 网卡
    std::vector<NicInfo> nics_;
    size_t selectedNic_ = 0;

    // L2 Transport
    std::unique_ptr<L2Transport> transport_;
    bool transportRunning_ = false;

    // 设备发现
    std::vector<DiscoveredDevice> devices_;
    std::mutex devicesMutex_;
    std::chrono::steady_clock::time_point lastScanTime_;
    std::chrono::steady_clock::time_point lastRefreshClickTime_;
    std::chrono::steady_clock::time_point lastScanClickTime_;
    l2config::FragmentAssembler discoveryAssembler_;

    // SSH 隧道
    std::unique_ptr<L2TcpTunnel> tunnel_;
    TunnelState tunnelState_ = TunnelState::IDLE;
    std::string tunnelTargetMac_;
    std::string tunnelError_;
    int tunnelLocalPort_ = 2222;
    std::thread tunnelThread_;
};

}  // namespace l2config_tool
