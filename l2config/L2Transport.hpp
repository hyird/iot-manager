#pragma once

/**
 * @brief Npcap 封装层 - Windows 上的 L2 原始以太网帧收发
 *
 * 使用 Npcap (libpcap) 打开网卡、设置 BPF 过滤器，
 * 在独立线程中接收帧并通过回调处理。
 */

#include "common/l2config/L2Protocol.hpp"

#include <winsock2.h>
#include <iphlpapi.h>
#include <pcap.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace l2config_tool {

struct NicInfo {
    std::string pcapName;    // pcap 设备名 (如 \Device\NPF_{GUID})
    std::string displayName; // 友好名称 (如 "以太网")
    std::string adapterName; // Windows 适配器名 ({GUID})
    l2config::MacAddr mac{};
    std::string ip;
    bool up = false;
};

class L2Transport {
public:
    using FrameCallback = std::function<void(const uint8_t* data, size_t len)>;

    L2Transport() = default;
    ~L2Transport() { stop(); }

    L2Transport(const L2Transport&) = delete;
    L2Transport& operator=(const L2Transport&) = delete;

    /**
     * @brief 列举系统上所有可用的 Npcap 网卡
     */
    static std::vector<NicInfo> listInterfaces() {
        std::vector<NicInfo> result;

        // 先通过 Windows API 获取适配器信息（MAC、友好名称等）
        std::map<std::string, NicInfo> adapterMap; // GUID → info
        {
            ULONG bufSize = 15000;
            std::vector<uint8_t> buf(bufSize);
            auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());

            ULONG ret = GetAdaptersAddresses(
                AF_INET,
                GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                nullptr, addrs, &bufSize);

            if (ret == ERROR_BUFFER_OVERFLOW) {
                buf.resize(bufSize);
                addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
                ret = GetAdaptersAddresses(
                    AF_INET,
                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                    nullptr, addrs, &bufSize);
            }

            if (ret == NO_ERROR) {
                for (auto* a = addrs; a; a = a->Next) {
                    if (a->IfType != IF_TYPE_ETHERNET_CSMACD &&
                        a->IfType != IF_TYPE_IEEE80211) {
                        continue;
                    }

                    NicInfo info;
                    info.adapterName = a->AdapterName;
                    info.up = (a->OperStatus == IfOperStatusUp);

                    // MAC
                    if (a->PhysicalAddressLength == 6) {
                        std::memcpy(info.mac.data(), a->PhysicalAddress, 6);
                    }

                    // 友好名称
                    if (a->FriendlyName) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                                      nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                            std::string name(len - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                                name.data(), len, nullptr, nullptr);
                            info.displayName = name;
                        }
                    }

                    // IP
                    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                        if (u->Address.lpSockaddr->sa_family == AF_INET) {
                            auto* sa = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
                            char ipBuf[INET_ADDRSTRLEN];
                            if (inet_ntop(AF_INET, &sa->sin_addr, ipBuf, sizeof(ipBuf))) {
                                info.ip = ipBuf;
                            }
                            break;
                        }
                    }

                    adapterMap[info.adapterName] = info;
                }
            }
        }

        // 通过 pcap 获取设备列表，与 Windows 适配器信息匹配
        pcap_if_t* alldevs = nullptr;
        char errbuf[PCAP_ERRBUF_SIZE];
        if (pcap_findalldevs(&alldevs, errbuf) < 0) {
            return result;
        }

        for (auto* d = alldevs; d; d = d->next) {
            std::string pcapName = d->name;

            // pcap 设备名格式: \Device\NPF_{GUID}
            // Windows 适配器名格式: {GUID}
            std::string guid;
            auto pos = pcapName.find('{');
            if (pos != std::string::npos) {
                guid = pcapName.substr(pos);
            }

            auto it = adapterMap.find(guid);
            if (it != adapterMap.end()) {
                NicInfo info = it->second;
                info.pcapName = pcapName;
                result.push_back(info);
            }
        }

        pcap_freealldevs(alldevs);
        return result;
    }

    /**
     * @brief 通过友好名称或 MAC 找到对应的 pcap 设备
     */
    static std::optional<NicInfo> findInterface(const std::string& nameOrMac) {
        auto nics = listInterfaces();
        for (const auto& nic : nics) {
            if (nic.displayName == nameOrMac ||
                nic.pcapName == nameOrMac ||
                nic.adapterName == nameOrMac ||
                l2config::macToString(nic.mac) == nameOrMac) {
                return nic;
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 打开指定 pcap 设备并开始接收
     * @param pcapDeviceName pcap 设备名
     * @return 错误信息，空字符串表示成功
     */
    std::string start(const std::string& pcapDeviceName, const l2config::MacAddr& mac) {
        if (running_) return "";

        char errbuf[PCAP_ERRBUF_SIZE];
        handle_ = pcap_open_live(pcapDeviceName.c_str(), 65536, 0, 1, errbuf);
        if (!handle_) {
            return std::string("pcap_open_live failed: ") + errbuf;
        }

        // 设置 BPF 过滤器：只接收 EtherType = 0x88B5 的帧
        bpf_program fp{};
        if (pcap_compile(handle_, &fp, "ether proto 0x88b5", 1, PCAP_NETMASK_UNKNOWN) < 0) {
            pcap_close(handle_);
            handle_ = nullptr;
            return "pcap_compile failed";
        }
        if (pcap_setfilter(handle_, &fp) < 0) {
            pcap_freecode(&fp);
            pcap_close(handle_);
            handle_ = nullptr;
            return "pcap_setfilter failed";
        }
        pcap_freecode(&fp);

        localMac_ = mac;
        running_ = true;
        recvThread_ = std::thread([this]() { recvLoop(); });

        return "";
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        if (handle_) {
            pcap_breakloop(handle_);
        }

        if (recvThread_.joinable()) {
            recvThread_.join();
        }

        if (handle_) {
            pcap_close(handle_);
            handle_ = nullptr;
        }
    }

    bool isRunning() const { return running_; }

    void setFrameCallback(FrameCallback cb) {
        frameCallback_ = std::move(cb);
    }

    /**
     * @brief 发送原始以太网帧
     */
    bool sendFrame(const std::vector<uint8_t>& frame) {
        if (!handle_ || !running_) return false;
        std::lock_guard<std::mutex> lock(sendMutex_);
        return pcap_sendpacket(handle_, frame.data(), static_cast<int>(frame.size())) == 0;
    }

    bool sendFrames(const std::vector<std::vector<uint8_t>>& frames) {
        for (const auto& f : frames) {
            if (!sendFrame(f)) return false;
        }
        return true;
    }

    const l2config::MacAddr& localMac() const { return localMac_; }

private:
    void recvLoop() {
        while (running_) {
            pcap_pkthdr* header = nullptr;
            const uint8_t* data = nullptr;
            int ret = pcap_next_ex(handle_, &header, &data);
            if (ret == 1 && data && header->caplen > 0) {
                if (frameCallback_) {
                    frameCallback_(data, header->caplen);
                }
            } else if (ret == -2) {
                // pcap_breakloop was called
                break;
            } else if (ret == -1) {
                if (running_) {
                    std::cerr << "[L2Transport] pcap error: " << pcap_geterr(handle_) << std::endl;
                }
                break;
            }
            // ret == 0: timeout, continue
        }
    }

    pcap_t* handle_ = nullptr;
    l2config::MacAddr localMac_{};
    std::atomic<bool> running_{false};
    std::thread recvThread_;
    FrameCallback frameCallback_;
    std::mutex sendMutex_;
};

}  // namespace l2config_tool
