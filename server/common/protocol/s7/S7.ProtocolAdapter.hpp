#pragma once

#include "S7.DtuRegistry.hpp"
#include "S7.DtuSessionManager.hpp"
#include "S7.RegistrationNormalizer.hpp"
#include "S7.Client.hpp"
#include "S7.PollScheduler.hpp"
#include "common/cache/DeviceCache.hpp"
#include "common/network/LinkTransportFacade.hpp"
#include "common/protocol/ProtocolAdapter.hpp"
#include "common/protocol/ProtocolJobQueue.hpp"
#include "common/utils/Constants.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace s7 {

inline constexpr int kDefaultS7SendTimeoutMs = kDefaultTimeoutMs;
inline constexpr int kDefaultS7RecvTimeoutMs = 15000;

struct S7AreaDefinition {
    std::string id;
    std::string name;
    std::string area = "DB";
    std::string dataType = "INT16";
    int dbNumber = 0;
    int start = 0;
    int startBit = 0;
    int size = 1;
    bool writable = false;
    std::string remark;
};

struct S7ConnectionConfig {
    std::string host;  // 解析后的连接目标，优先取链路 IP，兼容旧配置
    std::string mode = "RACK_SLOT";
    int rack = 0;
    int slot = 1;
    std::uint16_t localTSAP = 0x0100;
    std::uint16_t remoteTSAP = 0x0100;
    int pingTimeoutMs = kDefaultS7RecvTimeoutMs;
    int sendTimeoutMs = kDefaultS7SendTimeoutMs;
    int recvTimeoutMs = kDefaultS7RecvTimeoutMs;
    int retryDelayMs = 1000;
    int pollIntervalSec = 5;
    std::uint16_t pduRequestLength = kDefaultS7PduRequest;
    std::string connectionType = "PG";
};

inline bool sameS7Connection(const S7ConnectionConfig& lhs, const S7ConnectionConfig& rhs) {
    return lhs.host == rhs.host
        && lhs.mode == rhs.mode
        && lhs.rack == rhs.rack
        && lhs.slot == rhs.slot
        && lhs.localTSAP == rhs.localTSAP
        && lhs.remoteTSAP == rhs.remoteTSAP
        && lhs.pingTimeoutMs == rhs.pingTimeoutMs
        && lhs.sendTimeoutMs == rhs.sendTimeoutMs
        && lhs.recvTimeoutMs == rhs.recvTimeoutMs
        && lhs.retryDelayMs == rhs.retryDelayMs
        && lhs.pollIntervalSec == rhs.pollIntervalSec
        && lhs.pduRequestLength == rhs.pduRequestLength
        && lhs.connectionType == rhs.connectionType;
}

struct S7ConnectionPreset {
    std::string mode = "RACK_SLOT";
    int rack = 0;
    int slot = 1;
    std::uint16_t localTSAP = 0x0100;
    std::uint16_t remoteTSAP = 0x0100;
};

struct S7ReadBlockMember {
    const S7AreaDefinition* area = nullptr;
    std::size_t offsetBytes = 0;
};

struct S7ReadBlockPlan {
    std::string area;
    int areaCode = S7AreaDB;
    int wordLen = S7WLByte;
    int unitSize = 1;
    int dbNumber = 0;
    int start = 0;
    int amount = 0;
    std::size_t byteSize = 0;
    std::vector<S7ReadBlockMember> members;
};

struct S7DeviceRuntime {
    struct AsyncConnect {
        enum class Stage {
            IsoConfirm,
            SetupCommunication
        };

        Stage stage = Stage::IsoConfirm;
        std::uint64_t attemptId = 0;
        std::deque<uint8_t> rxBuffer;
        std::function<void(bool)> complete;
        trantor::TimerId timerId{0};
        bool done = false;
    };

    struct AsyncExchange {
        std::uint16_t pduRef = 0;
        std::uint8_t expectedFunction = 0;
        std::uint8_t expectedCotp = kCotpDt;
        bool rawFrame = false;
        std::string responseStage;
        std::function<void(int, std::vector<std::uint8_t>)> complete;
        trantor::TimerId timerId{0};
        bool done = false;
    };

    struct QueuedOperation {
        ProtocolJobPriority priority = ProtocolJobPriority::Normal;
        std::function<void(std::function<void()>)> run;
    };

    mutable std::mutex mutex;
    mutable std::mutex clientMutex;
    int deviceId = 0;
    int linkId = 0;
    std::string deviceName;
    std::string deviceCode;
    std::string sessionKey;
    std::string sessionClientAddr;
    int sessionLinkId = 0;
    S7ConnectionConfig connection;
    std::vector<S7AreaDefinition> areas;
    std::deque<uint8_t> asyncRxBuffer;
    std::shared_ptr<AsyncConnect> asyncConnect;
    std::shared_ptr<AsyncExchange> asyncExchange;
    ProtocolJobQueue<QueuedOperation> operationQueue{256};
    std::unique_ptr<Client> client;
    bool connected = false;
    bool tcpServerMode = false;
    bool sessionBound = false;
    bool sessionDiscoveryInFlight = false;
    bool connectInProgress = false;
    bool operationRunning = false;
    std::uint64_t connectGeneration = 0;
    std::size_t sourceRefIndex = 0;
    std::chrono::steady_clock::time_point sessionDiscoveryStartedAt{};
    std::chrono::steady_clock::time_point lastConnectAttempt{};

    S7DeviceRuntime() = default;

    ~S7DeviceRuntime() {
        resetClient();
    }

    S7DeviceRuntime(const S7DeviceRuntime&) = delete;
    S7DeviceRuntime& operator=(const S7DeviceRuntime&) = delete;

    S7DeviceRuntime(S7DeviceRuntime&& other) noexcept
        : deviceId(other.deviceId)
        , linkId(other.linkId)
        , deviceName(std::move(other.deviceName))
        , deviceCode(std::move(other.deviceCode))
        , sessionKey(std::move(other.sessionKey))
        , sessionClientAddr(std::move(other.sessionClientAddr))
        , sessionLinkId(other.sessionLinkId)
        , connection(std::move(other.connection))
        , areas(std::move(other.areas))
        , asyncRxBuffer(std::move(other.asyncRxBuffer))
        , asyncConnect(std::move(other.asyncConnect))
        , asyncExchange(std::move(other.asyncExchange))
        , operationQueue(std::move(other.operationQueue))
        , client(std::move(other.client))
        , connected(other.connected)
        , tcpServerMode(other.tcpServerMode)
        , sessionBound(other.sessionBound)
        , sessionDiscoveryInFlight(other.sessionDiscoveryInFlight)
        , connectInProgress(other.connectInProgress)
        , operationRunning(other.operationRunning)
        , connectGeneration(other.connectGeneration)
        , sourceRefIndex(other.sourceRefIndex)
        , sessionDiscoveryStartedAt(other.sessionDiscoveryStartedAt)
        , lastConnectAttempt(other.lastConnectAttempt) {
        other.connected = false;
    }

    S7DeviceRuntime& operator=(S7DeviceRuntime&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        resetClient();

        deviceId = other.deviceId;
        linkId = other.linkId;
        deviceName = std::move(other.deviceName);
        deviceCode = std::move(other.deviceCode);
        sessionKey = std::move(other.sessionKey);
        sessionClientAddr = std::move(other.sessionClientAddr);
        sessionLinkId = other.sessionLinkId;
        connection = std::move(other.connection);
        areas = std::move(other.areas);
        asyncRxBuffer = std::move(other.asyncRxBuffer);
        asyncConnect = std::move(other.asyncConnect);
        asyncExchange = std::move(other.asyncExchange);
        operationQueue = std::move(other.operationQueue);
        client = std::move(other.client);
        connected = other.connected;
        tcpServerMode = other.tcpServerMode;
        sessionBound = other.sessionBound;
        sessionDiscoveryInFlight = other.sessionDiscoveryInFlight;
        connectInProgress = other.connectInProgress;
        operationRunning = other.operationRunning;
        connectGeneration = other.connectGeneration;
        sourceRefIndex = other.sourceRefIndex;
        sessionDiscoveryStartedAt = other.sessionDiscoveryStartedAt;
        lastConnectAttempt = other.lastConnectAttempt;
        other.connected = false;
        return *this;
    }

    void resetClient() {
        if (client) {
            client->disconnect();
            client.reset();
        }
        asyncRxBuffer.clear();
        asyncConnect.reset();
        asyncExchange.reset();
        operationQueue.clear();
        operationRunning = false;
        connected = false;
    }
};

/**
 * @brief S7 协议适配器
 *
 * 运行模型是“单设备 runtime + session binding + 轮询调度”：
 * - 每个 device 独立维护 runtime
 * - session 模式下用 session 绑定连接和设备
 * - reload 时重建 runtime 列表和轮询配置
 */
class S7ProtocolAdapter final : public ProtocolAdapter {
public:
    explicit S7ProtocolAdapter(ProtocolRuntimeContext runtimeContext)
        : ProtocolAdapter(std::move(runtimeContext)) {
        sessionRegistry_ = std::make_unique<DtuRegistry>();
        sessionManager_ = std::make_unique<DtuSessionManager>();
        sessionNormalizer_ = std::make_unique<RegistrationNormalizer>(*sessionRegistry_, *sessionManager_);
        pollScheduler_ = std::make_unique<S7PollScheduler>();
        pollScheduler_->setEnqueuePollCallback([this](int deviceId) {
            return enqueueScheduledPoll(deviceId);
        });
        sessionManager_->setOldSessionDisplacedCallback(
            [](int linkId, const std::string& clientAddr) {
                LinkTransportFacade::instance().disconnectServerClient(linkId, clientAddr);
            }
        );
    }

    std::string_view protocol() const override {
        return Constants::PROTOCOL_S7;
    }

    Task<> initializeAsync() override {
        co_await refreshDevices();
        co_return;
    }

    Task<> reloadAsync() override {
        co_await refreshDevices();
        co_return;
    }

    void onConnectionChanged(int linkId, const std::string& clientAddr, bool connected) override {
        std::optional<S7DtuSession> previousSession;
        if (sessionManager_) {
            previousSession = sessionManager_->getSession(linkId, clientAddr);
        }

        if (sessionManager_) {
            if (connected) {
                sessionManager_->onConnected(linkId, clientAddr);
            } else {
                sessionManager_->onDisconnected(linkId, clientAddr);
            }
        }

        if (!sessionRegistry_ || sessionRegistry_->empty()) {
            return;
        }

        auto definitions = sessionRegistry_->getDefinitionsByLink(linkId);
        if (definitions.empty()) {
            return;
        }

        LOG_INFO << "[S7][Adapter] Session link " << linkId
                 << (connected ? " connected: " : " disconnected: ")
                 << clientAddr;

        if (!connected && previousSession) {
            if (previousSession->bindState == SessionBindState::Bound
                && previousSession->deviceId > 0) {
                closeRuntimeClient(previousSession->deviceId);
            } else if (previousSession->bindState == SessionBindState::Probing
                       && previousSession->probingDeviceId > 0) {
                closeRuntimeClient(previousSession->probingDeviceId);
            }
        }
        if (connected) {
            triggerLinkDiscoveryNow(linkId);
        }
    }

    void onDataReceived(int linkId, const std::string& clientAddr, std::vector<uint8_t> bytes) override {
        if (!sessionNormalizer_ || !sessionRegistry_ || !sessionManager_) {
            return;
        }

        auto definitions = sessionRegistry_->getDefinitionsByLink(linkId);
        if (definitions.empty()) {
            return;
        }

        auto normalized = sessionNormalizer_->normalize(linkId, clientAddr, bytes);
        if (normalized.kind == RegistrationMatchKind::Conflict) {
            LOG_WARN << "[S7][Adapter] Registration conflict: linkId="
                     << linkId << ", client=" << clientAddr
                     << ", bytes=" << bytes.size();
            return;
        }

        logRegistrationMatch(linkId, clientAddr, normalized);

        int deviceId = 0;
        if (!normalized.dtuKey.empty()) {
            auto sessionDefOpt = sessionRegistry_->findByDtuKey(normalized.dtuKey);
            if (sessionDefOpt) {
                deviceId = sessionDefOpt->deviceId;
            }
        }

        auto sessionOpt = sessionManager_->getSession(linkId, clientAddr);
        if (deviceId <= 0 && sessionOpt && sessionOpt->bindState == SessionBindState::Bound) {
            deviceId = sessionOpt->deviceId;
        }

        if (deviceId > 0 && sessionOpt && sessionOpt->bindState == SessionBindState::Bound) {
            const std::string sessionKey = normalized.dtuKey.empty() ? sessionOpt->dtuKey : normalized.dtuKey;
            attachSessionBinding(deviceId, linkId, clientAddr, sessionKey);
        }

        if (!normalized.payload.empty() && deviceId > 0) {
            forwardDtuPayload(deviceId, std::move(normalized.payload));
            return;
        }

        if (!normalized.payload.empty()) {
            LOG_DEBUG << "[S7][Adapter] Drop unbound payload: linkId=" << linkId
                      << ", client=" << clientAddr
                      << ", bytes=" << normalized.payload.size()
                      << ", kind=" << registrationMatchKindToString(normalized.kind);
        }
    }

    void onMaintenanceTick() override {
    }

    ProtocolAdapterMetrics getMetrics() const override {
        ProtocolAdapterMetrics metrics;
        metrics.available = true;
        auto runtimes = snapshotRuntimesLocked();
        metrics.stats["deviceCount"] = static_cast<Json::Int64>(runtimes.size());
        Json::Int64 connectedCount = 0;
        Json::Int64 areaCount = 0;
        Json::Int64 sessionDeviceCount = 0;
        Json::Int64 sessionReadyCount = 0;
        for (const auto& runtime : runtimes) {
            std::lock_guard runtimeLock(runtime->mutex);
            if (runtime->tcpServerMode) {
                ++sessionDeviceCount;
            }
            if (runtime->connected && (!runtime->tcpServerMode || isSessionReadyLocked(*runtime))) {
                ++connectedCount;
            }
            areaCount += static_cast<Json::Int64>(runtime->areas.size());
            if (runtime->tcpServerMode && isSessionReadyLocked(*runtime)) {
                ++sessionReadyCount;
            }
        }
        metrics.stats["connectedCount"] = connectedCount;
        metrics.stats["areaCount"] = areaCount;
        metrics.stats["sessionDeviceCount"] = sessionDeviceCount;
        metrics.stats["sessionReadyCount"] = sessionReadyCount;
        return metrics;
    }

    ProtocolLifecycleImpact onDeviceLifecycleEvent(const DeviceLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_S7) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    ProtocolLifecycleImpact onProtocolConfigLifecycleEvent(const ProtocolConfigLifecycleEvent& event) override {
        if (!event.protocol.empty() && event.protocol != Constants::PROTOCOL_S7) {
            return ProtocolLifecycleImpact::None;
        }
        return ProtocolLifecycleImpact::Reload;
    }

    bool isDeviceConnected(int deviceId) const override {
        auto runtime = const_cast<S7ProtocolAdapter*>(this)->findRuntimeLocked(deviceId);
        if (!runtime) {
            return false;
        }

        std::lock_guard runtimeLock(runtime->mutex);
        return runtime->connected
            && (!runtime->tcpServerMode || isSessionReadyLocked(*runtime));
    }

    Task<CommandResult> sendCommand(const CommandRequest& req) override {
        auto guard = makeCommandGuard();

        try {
            auto deviceOpt = req.deviceId > 0
                ? DeviceCache::instance().findByIdSync(req.deviceId)
                : std::optional<DeviceCache::CachedDevice>{};
            if (!deviceOpt && !req.deviceCode.empty()) {
                deviceOpt = DeviceCache::instance().findByCodeSync(req.deviceCode);
            }
            if (!deviceOpt || deviceOpt->protocolType != Constants::PROTOCOL_S7) {
                co_return CommandResult::error("S7 设备不存在");
            }

            const int deviceId = deviceOpt->id;
            int linkId = deviceOpt->linkId;
            std::string deviceCode = deviceOpt->deviceCode.empty()
                ? std::to_string(deviceOpt->id)
                : deviceOpt->deviceCode;

            auto runtime = findRuntimeLocked(deviceId);
            if (!runtime) {
                co_return CommandResult::offline("S7 设备离线");
            }

            {
                std::lock_guard runtimeLock(runtime->mutex);
                linkId = runtime->linkId;
                if (!runtime->deviceCode.empty()) {
                    deviceCode = runtime->deviceCode;
                }
            }

            if (!req.elements.isArray() || req.elements.empty()) {
                co_return CommandResult::error("S7 指令缺少 elements");
            }

            int64_t downCommandId = co_await savePendingCommand(
                deviceId, linkId, Constants::PROTOCOL_S7,
                req.funcCode, "S7 写入", "", req.userId, req.elements);

            bool useAsyncDeviceQueue = false;
            {
                std::lock_guard runtimeLock(runtime->mutex);
                useAsyncDeviceQueue = runtime->tcpServerMode;
            }

            if (useAsyncDeviceQueue) {
                auto failure = co_await AsyncStringAwaiter(
                    [this, runtime, elements = req.elements](std::function<void(std::string)> complete) mutable {
                        auto completePtr = std::make_shared<std::function<void(std::string)>>(std::move(complete));
                        if (!enqueueDeviceOperation(
                            runtime,
                            ProtocolJobPriority::High,
                            [this, runtime, elements = std::move(elements), completePtr](
                                std::function<void()> done) mutable {
                                auto elementsPtr = std::make_shared<Json::Value>(std::move(elements));
                                startAsyncConnectDevice(
                                    runtime,
                                    [this, runtime, elementsPtr, done = std::move(done), completePtr](bool connected) mutable {
                                        if (!connected) {
                                            if (completePtr && *completePtr) {
                                                (*completePtr)("S7 设备离线");
                                            }
                                            if (done) {
                                                done();
                                            }
                                            return;
                                        }
                                        startAsyncWriteCommand(
                                            runtime,
                                            std::move(*elementsPtr),
                                            [done = std::move(done), completePtr](std::string failure) mutable {
                                                if (completePtr && *completePtr) {
                                                    (*completePtr)(std::move(failure));
                                                }
                                                if (done) {
                                                    done();
                                                }
                                            });
                                    });
                            })) {
                            if (completePtr && *completePtr) {
                                (*completePtr)("S7 设备队列已满");
                            }
                        }
                    });

                if (!failure.empty()) {
                    co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SEND_FAILED", failure);
                    co_return CommandResult::sendFailed(failure);
                }

                co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SUCCESS", "S7 写入成功");
                if (runtimeContext_.notifyCommandCompletion) {
                    runtimeContext_.notifyCommandCompletion(deviceCode,
                        "SUCCESS", true, downCommandId);
                }
                if (pollScheduler_) {
                    pollScheduler_->activateFastRead(
                        deviceId,
                        deviceOpt->commandFastReadDuration,
                        deviceOpt->commandFastReadInterval
                    );
                }
                guard.release();
                co_return CommandResult::success();
            }

            auto failure = std::string("S7 直连 TCP 非阻塞客户端尚未启用");
            co_await runtimeContext_.commandStore.updateCommandStatus(downCommandId, "SEND_FAILED", failure);
            co_return CommandResult::sendFailed(failure);
        } catch (const std::exception& e) {
            co_return CommandResult::error(e.what());
        }
    }

private:
    inline static constexpr const char* FUNC_READ = "S7_READ";
    inline static constexpr std::size_t kMaxPendingSessionFrames = 64;

    static std::string toUpper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }

    static std::string normalizeDataType(std::string value) {
        value = toUpper(std::move(value));
        if (value == "DOUBLE") return "LREAL";
        if (value == "BOOL" || value == "INT8" || value == "UINT8" || value == "INT16" ||
            value == "UINT16" || value == "INT32" || value == "UINT32" || value == "FLOAT" ||
            value == "LREAL" || value == "STRING") {
            return value;
        }
        return "INT16";
    }

    static int areaToCode(const std::string& area) {
        const std::string upper = toUpper(area);
        if (upper == "DB") return S7AreaDB;
        if (upper == "V") return S7AreaDB;
        if (upper == "MK") return S7AreaMK;
        if (upper == "PA") return S7AreaPA;
        if (upper == "PE") return S7AreaPE;
        if (upper == "CT") return S7AreaCT;
        if (upper == "TM") return S7AreaTM;
        return S7AreaDB;
    }

    static int areaWordLen(const std::string& area) {
        const std::string upper = toUpper(area);
        if (upper == "CT") return S7WLCounter;
        if (upper == "TM") return S7WLTimer;
        return S7WLByte;
    }

    static int transferAmount(const std::string& area, size_t size) {
        const std::string upper = toUpper(area);
        if (upper == "CT" || upper == "TM") {
            return std::max(1, static_cast<int>((size + 1) / 2));
        }
        return static_cast<int>(size);
    }

    static int resolvedDbNumber(const S7AreaDefinition& area) {
        return area.area == "V" ? 1 : area.dbNumber;
    }

    static int areaUnitSize(const std::string& area) {
        return wordLenByteSize(areaWordLen(area));
    }

    static int areaSpanUnits(const S7AreaDefinition& area) {
        return transferAmount(area.area, static_cast<std::size_t>(area.size));
    }

    struct S7PreparedWrite {
        const S7AreaDefinition* area = nullptr;
        Json::Value element;
        std::vector<uint8_t> existingBytes;
        std::vector<uint8_t> writeBytes;
    };

    struct AsyncPollContext {
        int deviceId = 0;
        int linkId = 0;
        std::string reportTime;
        std::vector<S7ReadBlockPlan> plans;
        std::vector<std::vector<uint8_t>> buffers;
        std::vector<DataItem> items;
        std::vector<std::size_t> indexes;
        std::vector<int> results;
        std::vector<std::pair<std::size_t, std::size_t>> batches;
        std::size_t nextBatch = 0;
        bool pollFailed = false;
    };

    struct AsyncCommandWriteContext {
        std::vector<S7PreparedWrite> writes;
        std::vector<DataItem> readItems;
        std::vector<std::pair<std::size_t, std::size_t>> readBatches;
        std::size_t nextReadBatch = 0;
        std::size_t nextWrite = 0;
    };

    struct LinkQueuedOperation {
        std::function<void(std::function<void()>)> run;
    };

    struct LinkOperationRuntime {
        ProtocolJobQueue<LinkQueuedOperation> queue{256};
        bool running = false;
    };

    class AsyncStringAwaiter {
    public:
        explicit AsyncStringAwaiter(std::function<void(std::function<void(std::string)>)> starter)
            : starter_(std::move(starter)) {}

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            auto* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
            starter_([this, handle, loop](std::string value) mutable {
                result_ = std::move(value);
                if (loop) {
                    loop->queueInLoop([handle]() mutable {
                        handle.resume();
                    });
                } else {
                    handle.resume();
                }
            });
        }

        std::string await_resume() {
            return std::move(result_);
        }

    private:
        std::function<void(std::function<void(std::string)>)> starter_;
        std::string result_;
    };

    static std::vector<S7ReadBlockPlan> planReadBlocks(const std::vector<S7AreaDefinition>& areas) {
        struct Candidate {
            const S7AreaDefinition* area = nullptr;
            int areaCode = S7AreaDB;
            int wordLen = S7WLByte;
            int unitSize = 1;
            int dbNumber = 0;
            int start = 0;
            int amount = 0;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(areas.size());
        for (const auto& area : areas) {
            if (area.size <= 0) {
                continue;
            }

            const int wordLen = areaWordLen(area.area);
            const int unitSize = wordLenByteSize(wordLen);
            const int amount = areaSpanUnits(area);
            if (unitSize <= 0 || amount <= 0) {
                continue;
            }

            candidates.push_back(Candidate{
                .area = &area,
                .areaCode = areaToCode(area.area),
                .wordLen = wordLen,
                .unitSize = unitSize,
                .dbNumber = resolvedDbNumber(area),
                .start = area.start,
                .amount = amount
            });
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
            if (lhs.areaCode != rhs.areaCode) return lhs.areaCode < rhs.areaCode;
            if (lhs.dbNumber != rhs.dbNumber) return lhs.dbNumber < rhs.dbNumber;
            if (lhs.wordLen != rhs.wordLen) return lhs.wordLen < rhs.wordLen;
            if (lhs.start != rhs.start) return lhs.start < rhs.start;
            return lhs.amount < rhs.amount;
        });

        constexpr int kMergeGapUnits = 0;
        std::vector<S7ReadBlockPlan> plans;
        std::optional<S7ReadBlockPlan> current;
        int currentEnd = 0;

        auto flushCurrent = [&]() {
            if (!current.has_value()) {
                return;
            }
            current->byteSize = static_cast<std::size_t>(current->amount) * current->unitSize;
            plans.push_back(std::move(*current));
            current.reset();
        };

        for (const auto& candidate : candidates) {
            const int candidateEnd = candidate.start + candidate.amount;
            const bool compatible = current.has_value()
                && candidate.areaCode == current->areaCode
                && candidate.dbNumber == current->dbNumber
                && candidate.wordLen == current->wordLen
                && candidate.start <= currentEnd + kMergeGapUnits;

            if (!compatible) {
                flushCurrent();
                current = S7ReadBlockPlan{
                    .area = candidate.area->area,
                    .areaCode = candidate.areaCode,
                    .wordLen = candidate.wordLen,
                    .unitSize = candidate.unitSize,
                    .dbNumber = candidate.dbNumber,
                    .start = candidate.start,
                    .amount = candidate.amount,
                    .members = {}
                };
                currentEnd = candidateEnd;
            } else if (candidateEnd > currentEnd) {
                currentEnd = candidateEnd;
                current->amount = currentEnd - current->start;
            }

            current->members.push_back(S7ReadBlockMember{
                .area = candidate.area,
                .offsetBytes = static_cast<std::size_t>(candidate.start - current->start) * current->unitSize
            });
        }

        flushCurrent();
        return plans;
    }

    static std::string normalizeConnectionMode(std::string value) {
        value = toUpper(std::move(value));
        if (value == "TSAP") {
            return "TSAP";
        }
        return "RACK_SLOT";
    }

    static std::uint16_t connectionTypeToCode(std::string value) {
        value = toUpper(std::move(value));
        if (value == "OP") {
            return kConnTypeOp;
        }
        if (value == "S7_BASIC" || value == "BASIC") {
            return kConnTypeBasic;
        }
        return kConnTypePg;
    }

    static std::optional<std::uint16_t> parseTsapHex(const Json::Value& value) {
        if (value.isNull()) {
            return std::nullopt;
        }
        if (value.isInt() || value.isUInt()) {
            const unsigned int raw = value.asUInt();
            if (raw > 0xFFFF) {
                return std::nullopt;
            }
            return static_cast<std::uint16_t>(raw);
        }
        if (!value.isString()) {
            return std::nullopt;
        }

        std::string text = value.asString();
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c) || c == '.' || c == ':' || c == '-' || c == '_';
        }), text.end());
        text = toUpper(std::move(text));
        if (text.rfind("0X", 0) == 0) {
            text.erase(0, 2);
        }
        if (text.empty() || text.size() > 4) {
            return std::nullopt;
        }
        if (!std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::stoul(text, nullptr, 16));
    }

    static std::optional<S7ConnectionPreset> resolvePreset(const std::string& plcModel) {
        if (plcModel == "S7-200") return S7ConnectionPreset{.mode = "TSAP", .rack = 0, .slot = 1, .localTSAP = 0x4D57, .remoteTSAP = 0x4D57};
        if (plcModel == "S7-300") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 2};
        if (plcModel == "S7-400") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 3};
        if (plcModel == "S7-1200" || plcModel == "S7-1500") return S7ConnectionPreset{.mode = "RACK_SLOT", .rack = 0, .slot = 1};
        return std::nullopt;
    }

    static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) {
            return {};
        }

        std::ostringstream oss;
        oss << std::hex << std::uppercase << std::setfill('0');
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0) {
                oss << ' ';
            }
            oss << std::setw(2) << static_cast<int>(bytes[i]);
        }
        return oss.str();
    }

    static std::string bytesToAsciiString(const std::vector<uint8_t>& bytes) {
        static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";

        std::string out;
        out.reserve(bytes.size());
        for (uint8_t byte : bytes) {
            switch (byte) {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (byte >= 0x20 && byte <= 0x7E) {
                        out.push_back(static_cast<char>(byte));
                    } else {
                        out += "\\x";
                        out.push_back(HEX_DIGITS[byte >> 4]);
                        out.push_back(HEX_DIGITS[byte & 0x0F]);
                    }
                    break;
            }
        }
        return out;
    }

    static std::string hexByte(uint8_t value) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(2) << static_cast<int>(value);
        return oss.str();
    }

    static std::string hexWord(uint16_t value) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(4) << value;
        return oss.str();
    }

    static std::optional<uint16_t> readU16BEAt(const std::vector<uint8_t>& bytes, std::size_t offset) {
        if (offset + 1 >= bytes.size()) {
            return std::nullopt;
        }
        return static_cast<uint16_t>((static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
    }

    static std::optional<uint16_t> readU16LEAt(const std::vector<uint8_t>& bytes, std::size_t offset) {
        if (offset + 1 >= bytes.size()) {
            return std::nullopt;
        }
        return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset])
            | (static_cast<uint16_t>(bytes[offset + 1]) << 8));
    }

    static std::optional<uint32_t> readU24BEAt(const std::vector<uint8_t>& bytes, std::size_t offset) {
        if (offset + 2 >= bytes.size()) {
            return std::nullopt;
        }
        return (static_cast<uint32_t>(bytes[offset]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
            | static_cast<uint32_t>(bytes[offset + 2]);
    }

    static std::string bytesToHexLimited(const std::vector<uint8_t>& bytes, std::size_t maxBytes = 16) {
        if (bytes.size() <= maxBytes) {
            return bytesToHex(bytes);
        }
        std::vector<uint8_t> prefix(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(maxBytes));
        return bytesToHex(prefix) + " ...";
    }

    static std::string sanitizeUtf8(std::string_view input) {
        std::string output;
        output.reserve(input.size());
        for (std::size_t i = 0; i < input.size();) {
            const auto c = static_cast<unsigned char>(input[i]);
            if (c < 0x80) {
                output.push_back(static_cast<char>(c));
                ++i;
                continue;
            }

            std::size_t length = 0;
            uint32_t codepoint = 0;
            if ((c & 0xE0) == 0xC0) {
                length = 2;
                codepoint = c & 0x1F;
            } else if ((c & 0xF0) == 0xE0) {
                length = 3;
                codepoint = c & 0x0F;
            } else if ((c & 0xF8) == 0xF0) {
                length = 4;
                codepoint = c & 0x07;
            } else {
                output.push_back('?');
                ++i;
                continue;
            }

            if (i + length > input.size()) {
                output.push_back('?');
                break;
            }

            bool valid = true;
            for (std::size_t j = 1; j < length; ++j) {
                const auto cc = static_cast<unsigned char>(input[i + j]);
                if ((cc & 0xC0) != 0x80) {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6) | (cc & 0x3F);
            }

            const bool overlong = (length == 2 && codepoint < 0x80)
                || (length == 3 && codepoint < 0x800)
                || (length == 4 && codepoint < 0x10000);
            const bool invalidCodepoint = codepoint > 0x10FFFF
                || (codepoint >= 0xD800 && codepoint <= 0xDFFF);
            if (!valid || overlong || invalidCodepoint) {
                output.push_back('?');
                ++i;
                continue;
            }

            output.append(input.substr(i, length));
            i += length;
        }
        return output;
    }

    static const char* transportSizeName(uint8_t transportSize) {
        switch (transportSize) {
            case kTsResBit:
                return "BIT";
            case kTsResByte:
                return "BYTE";
            case kTsResInt:
                return "INT";
            case kTsResReal:
                return "REAL";
            case kTsResOctet:
                return "OCTET";
            default:
                return nullptr;
        }
    }

    static const char* wordLenName(uint8_t wordLen) {
        switch (wordLen) {
            case S7WLBit:
                return "BIT";
            case S7WLByte:
                return "BYTE";
            case S7WLChar:
                return "CHAR";
            case S7WLWord:
                return "WORD";
            case S7WLInt:
                return "INT";
            case S7WLDWord:
                return "DWORD";
            case S7WLDInt:
                return "DINT";
            case S7WLReal:
                return "REAL";
            case S7WLCounter:
                return "COUNTER";
            case S7WLTimer:
                return "TIMER";
            default:
                return nullptr;
        }
    }

    static const char* areaCodeName(uint8_t areaCode) {
        switch (areaCode) {
            case S7AreaPE:
                return "PE";
            case S7AreaPA:
                return "PA";
            case S7AreaMK:
                return "MK";
            case S7AreaDB:
                return "DB";
            case S7AreaCT:
                return "CT";
            case S7AreaTM:
                return "TM";
            default:
                return nullptr;
        }
    }

    static std::size_t decodedTransportSize(uint8_t transportSize, uint16_t rawLength) {
        if (transportSize == kTsResOctet || transportSize == kTsResReal || transportSize == kTsResBit) {
            return rawLength;
        }
        return (static_cast<std::size_t>(rawLength) + 7) / 8;
    }

    static std::uint16_t decodeTpduSize(uint8_t encoded) {
        switch (encoded) {
            case 0x07: return 128;
            case 0x08: return 256;
            case 0x09: return 512;
            case 0x0A: return 1024;
            case 0x0B: return 2048;
            case 0x0C: return 4096;
            case 0x0D: return 8192;
            default: return 0;
        }
    }

    static uint32_t decodeS7StartAddress(uint8_t wordLen, uint32_t rawAddress) {
        if (wordLen == S7WLBit || wordLen == S7WLCounter || wordLen == S7WLTimer) {
            return rawAddress;
        }
        return rawAddress / 8;
    }

    static std::string summarizeIsoPacket(std::string_view stage, bool outbound, const std::vector<uint8_t>& frame) {
        const uint8_t expectedCode = outbound ? kCotpCr : kCotpCc;
        if (((outbound && stage != "iso.cr") || (!outbound && stage != "iso.cc"))
            || frame.size() < 11
            || frame[5] != expectedCode) {
            return {};
        }

        std::ostringstream oss;
        if (const auto dstRef = readU16BEAt(frame, 6)) {
            oss << " dstRef=" << hexWord(*dstRef);
        }
        if (const auto srcRef = readU16BEAt(frame, 8)) {
            oss << " srcRef=" << hexWord(*srcRef);
        }

        for (std::size_t i = 11; i + 1 < frame.size();) {
            const uint8_t code = frame[i];
            const uint8_t length = frame[i + 1];
            const std::size_t valueOffset = i + 2;
            const std::size_t next = valueOffset + length;
            if (next > frame.size()) {
                break;
            }

            if (code == 0xC0 && length == 1) {
                const auto tpduSize = decodeTpduSize(frame[valueOffset]);
                if (tpduSize > 0) {
                    oss << " tpduSize=" << tpduSize;
                }
            } else if (code == 0xC1 && length == 2) {
                if (const auto localTsap = readU16BEAt(frame, valueOffset)) {
                    oss << " localTsap=" << hexWord(*localTsap);
                }
            } else if (code == 0xC2 && length == 2) {
                if (const auto remoteTsap = readU16BEAt(frame, valueOffset)) {
                    oss << " remoteTsap=" << hexWord(*remoteTsap);
                }
            }

            i = next;
        }

        return oss.str();
    }

    static std::string summarizeS7Request(std::string_view stage, const std::vector<uint8_t>& frame) {
        if (frame.size() < 17 || frame[0] != kIsoTcpVersion || frame[5] != kCotpDt || frame[7] != kS7ProtocolId) {
            return {};
        }

        const auto pduRef = readU16LEAt(frame, 11);
        const auto parLen = readU16BEAt(frame, 13);
        const auto dataLen = readU16BEAt(frame, 15);
        if (!pduRef || !parLen || !dataLen) {
            return {};
        }

        std::ostringstream oss;
        oss << " pduRef=" << *pduRef;

        if (stage == "s7.setup-comm.req") {
            if (frame.size() >= 25) {
                if (const auto maxCalling = readU16BEAt(frame, 19)) {
                    oss << " maxCalling=" << *maxCalling;
                }
                if (const auto maxCalled = readU16BEAt(frame, 21)) {
                    oss << " maxCalled=" << *maxCalled;
                }
                if (const auto pduLength = readU16BEAt(frame, 23)) {
                    oss << " pduLength=" << *pduLength;
                }
            }
            return oss.str();
        }

        if (stage != "s7.read.req" && stage != "s7.write.req") {
            return {};
        }

        if (frame.size() >= 31) {
            const uint8_t itemCount = frame[18];
            oss << " items=" << static_cast<unsigned>(itemCount);
            if (itemCount > 0) {
                const uint8_t wordLen = frame[22];
                const auto amount = readU16BEAt(frame, 23);
                const auto dbNumber = readU16BEAt(frame, 25);
                const uint8_t areaCode = frame[27];
                const auto rawAddress = readU24BEAt(frame, 28);
                if (const char* area = areaCodeName(areaCode)) {
                    oss << " area=" << area;
                } else {
                    oss << " area=" << hexByte(areaCode);
                }
                if (areaCode == S7AreaDB && dbNumber) {
                    oss << " db=" << *dbNumber;
                }
                if (const char* name = wordLenName(wordLen)) {
                    oss << " wordLen=" << name;
                } else {
                    oss << " wordLen=" << hexByte(wordLen);
                }
                if (amount) {
                    oss << " amount=" << *amount;
                }
                if (rawAddress) {
                    oss << " start=" << decodeS7StartAddress(wordLen, *rawAddress);
                }
            }
        }

        if (stage == "s7.write.req") {
            const std::size_t dataOffset = 17 + static_cast<std::size_t>(*parLen);
            if (dataOffset + 4 <= frame.size()) {
                const uint8_t transportSize = frame[dataOffset + 1];
                if (const char* name = transportSizeName(transportSize)) {
                    oss << " transport=" << name;
                } else {
                    oss << " transport=" << hexByte(transportSize);
                }
                if (const auto rawLength = readU16BEAt(frame, dataOffset + 2)) {
                    const std::size_t payloadBytes = decodedTransportSize(transportSize, *rawLength);
                    oss << " dataBytes=" << payloadBytes;
                    const std::size_t payloadOffset = dataOffset + 4;
                    if (payloadOffset + payloadBytes <= frame.size()) {
                        std::vector<uint8_t> payload(
                            frame.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                            frame.begin() + static_cast<std::ptrdiff_t>(payloadOffset + payloadBytes));
                        oss << " dataHex=" << bytesToHexLimited(payload);
                    }
                }
            }
        }

        return oss.str();
    }

    static std::string summarizeS7Response(std::string_view stage, const std::vector<uint8_t>& frame) {
        if (frame.size() < 19 || frame[0] != kIsoTcpVersion || frame[5] != kCotpDt || frame[7] != kS7ProtocolId) {
            return {};
        }

        const auto pduRef = readU16LEAt(frame, 11);
        const auto parLen = readU16BEAt(frame, 13);
        const auto dataLen = readU16BEAt(frame, 15);
        const auto error = readU16BEAt(frame, 17);
        if (!pduRef || !parLen || !dataLen || !error) {
            return {};
        }

        std::ostringstream oss;
        oss << " pduRef=" << *pduRef
            << " error=" << hexWord(*error);

        if (stage == "s7.setup-comm.resp") {
            if (frame.size() >= 27) {
                if (const auto maxCalling = readU16BEAt(frame, 21)) {
                    oss << " maxCalling=" << *maxCalling;
                }
                if (const auto maxCalled = readU16BEAt(frame, 23)) {
                    oss << " maxCalled=" << *maxCalled;
                }
                if (const auto pduLength = readU16BEAt(frame, 25)) {
                    oss << " pduLength=" << *pduLength;
                }
            }
            return oss.str();
        }

        const std::size_t dataOffset = 19 + static_cast<std::size_t>(*parLen);
        if (dataOffset >= frame.size()) {
            return oss.str();
        }

        const uint8_t returnCode = frame[dataOffset];
        oss << " returnCode=" << (returnCode == 0xFF ? "OK" : hexByte(returnCode));

        if (stage == "s7.write.resp") {
            return oss.str();
        }

        if (dataOffset + 4 > frame.size()) {
            return oss.str();
        }

        const uint8_t transportSize = frame[dataOffset + 1];
        const auto payloadBitsOrBytes = readU16BEAt(frame, dataOffset + 2);
        if (!payloadBitsOrBytes) {
            return oss.str();
        }

        const std::size_t payloadBytes = decodedTransportSize(transportSize, *payloadBitsOrBytes);
        const std::size_t payloadOffset = dataOffset + 4;
        oss << " transport=";
        if (const char* name = transportSizeName(transportSize)) {
            oss << name;
        } else {
            oss << hexByte(transportSize);
        }
        oss << " dataBytes=" << payloadBytes;

        if (payloadOffset + payloadBytes <= frame.size()) {
            std::vector<uint8_t> payload(frame.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                                         frame.begin() + static_cast<std::ptrdiff_t>(payloadOffset + payloadBytes));
            oss << " dataHex=" << bytesToHexLimited(payload);
        }

        return oss.str();
    }

    static std::string summarizePacket(std::string_view stage, bool outbound, const std::vector<uint8_t>& frame) {
        if (const std::string isoSummary = summarizeIsoPacket(stage, outbound, frame); !isoSummary.empty()) {
            return isoSummary;
        }
        if (outbound) {
            return summarizeS7Request(stage, frame);
        }
        return summarizeS7Response(stage, frame);
    }

    static std::string explainPlcErrorRc(int rc) {
        switch (static_cast<std::uint16_t>(rc)) {
            case 0x0005:
                return "PLC/S7 error 0x0005 (Address out of range)";
            case 0x0006:
                return "PLC/S7 error 0x0006 (Invalid transport size)";
            case 0x0007:
                return "PLC/S7 error 0x0007 (Write data size mismatch)";
            case 0x000A:
                return "PLC/S7 error 0x000A (Item not available)";
            case 0x8104:
                return "PLC/S7 error 0x8104 (Function not available)";
            case 0x8500:
                return "PLC/S7 error 0x8500 (Data over PDU)";
            case 0xD209:
                return "PLC/S7 error 0xD209 (Item not available)";
            case 0xD241:
                return "PLC/S7 error 0xD241 (Need password)";
            case 0xD602:
                return "PLC/S7 error 0xD602 (Invalid password)";
            case 0xD604:
                return "PLC/S7 error 0xD604 (No password to clear)";
            case 0xD605:
                return "PLC/S7 error 0xD605 (No password to set)";
            case 0xDC01:
                return "PLC/S7 error 0xDC01 (Invalid value)";
            default:
                return "PLC/S7 error " + hexWord(static_cast<std::uint16_t>(rc));
        }
    }

    static std::string explainClientRc(int rc) {
        switch (rc) {
            case kS7Ok:
                return "OK";
            case kS7ErrInvalidHandle:
                return "Invalid handle";
            case kS7ErrInvalidParams:
                return "Invalid params";
            case kS7ErrSocketInit:
                return "Socket init failed";
            case kS7ErrResolveFailed:
                return "Resolve failed";
            case kS7ErrConnectFailed:
                return "Connect failed";
            case kS7ErrTimeout:
                return "Timeout";
            case kS7ErrSocketIo:
                return "Socket IO error";
            case kS7ErrProtocol:
                return "Protocol error";
            case kS7ErrNotConnected:
                return "Not connected";
            case kS7ErrPduNegotiation:
                return "PDU negotiation failed";
            case kS7ErrUnsupported:
                return "Unsupported";
            case kS7ErrResponseTooShort:
                return "Response too short";
            case kS7ErrSequenceMismatch:
                return "PDU reference mismatch";
            case kS7ErrFunctionMismatch:
                return "S7 function mismatch";
            case kS7ErrItemCountMismatch:
                return "S7 item count mismatch";
            case kS7ErrResponseLengthMismatch:
                return "S7 response length mismatch";
            default:
                return rc < 0 ? "Unknown client error" : explainPlcErrorRc(rc);
        }
    }

    static std::string registrationMatchKindToString(RegistrationMatchKind kind) {
        switch (kind) {
            case RegistrationMatchKind::StandaloneFrame:
                return "StandaloneFrame";
            case RegistrationMatchKind::PrefixedPayload:
                return "PrefixedPayload";
            case RegistrationMatchKind::Conflict:
                return "Conflict";
            case RegistrationMatchKind::None:
            default:
                return "None";
        }
    }

    static void logRegistrationMatch(
        int linkId,
        const std::string& clientAddr,
        const RegistrationMatchResult& normalized) {
        if (normalized.kind != RegistrationMatchKind::StandaloneFrame
            && normalized.kind != RegistrationMatchKind::PrefixedPayload) {
            return;
        }
        if (normalized.registrationBytes.empty()) {
            return;
        }

        LOG_DEBUG << "[S7][Adapter] Registration stripped: linkId=" << linkId
                  << ", client=" << clientAddr
                  << ", kind=" << registrationMatchKindToString(normalized.kind)
                  << ", hex=" << bytesToHex(normalized.registrationBytes)
                  << ", ascii=\"" << bytesToAsciiString(normalized.registrationBytes) << "\""
                  << ", payloadBytes=" << normalized.payload.size();
    }

    static std::string makeUtcNowString() {
        auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        std::chrono::hh_mm_ss hms{now - dp};

        std::ostringstream oss;
        oss << std::setfill('0')
            << std::setw(4) << static_cast<int>(ymd.year()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.month()) << "-"
            << std::setw(2) << static_cast<unsigned>(ymd.day()) << "T"
            << std::setw(2) << hms.hours().count() << ":"
            << std::setw(2) << hms.minutes().count() << ":"
            << std::setw(2) << hms.seconds().count() << "Z";
        return oss.str();
    }

    static uint16_t readU16BE(const std::vector<uint8_t>& buffer) {
        return static_cast<uint16_t>((static_cast<uint16_t>(buffer[0]) << 8) | buffer[1]);
    }

    static uint32_t readU32BE(const std::vector<uint8_t>& buffer) {
        return (static_cast<uint32_t>(buffer[0]) << 24)
            | (static_cast<uint32_t>(buffer[1]) << 16)
            | (static_cast<uint32_t>(buffer[2]) << 8)
            | static_cast<uint32_t>(buffer[3]);
    }

    static uint64_t readU64BE(const std::vector<uint8_t>& buffer) {
        return (static_cast<uint64_t>(buffer[0]) << 56)
            | (static_cast<uint64_t>(buffer[1]) << 48)
            | (static_cast<uint64_t>(buffer[2]) << 40)
            | (static_cast<uint64_t>(buffer[3]) << 32)
            | (static_cast<uint64_t>(buffer[4]) << 24)
            | (static_cast<uint64_t>(buffer[5]) << 16)
            | (static_cast<uint64_t>(buffer[6]) << 8)
            | static_cast<uint64_t>(buffer[7]);
    }

    static void appendU16BE(std::vector<uint8_t>& buffer, uint16_t value) {
        buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    static void appendU32BE(std::vector<uint8_t>& buffer, uint32_t value) {
        buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    static void appendU64BE(std::vector<uint8_t>& buffer, uint64_t value) {
        buffer.push_back(static_cast<uint8_t>((value >> 56) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 48) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 40) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 32) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    static Json::Value decodeAreaValue(const S7AreaDefinition& area, const std::vector<uint8_t>& buffer) {
        const std::string dataType = normalizeDataType(area.dataType);

        if ((area.area == "CT" || area.area == "TM") && buffer.size() >= 2) {
            return Json::Value(static_cast<Json::UInt64>(readU16BE(buffer)));
        }

        if (dataType == "BOOL") {
            if (buffer.empty()) {
                return Json::nullValue;
            }
            const int bit = std::clamp(area.startBit, 0, 7);
            return Json::Value((buffer[0] >> bit) & 0x01);
        }

        if (dataType == "INT8") {
            if (buffer.empty()) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int8_t>(buffer[0])));
        }
        if (dataType == "UINT8") {
            if (buffer.empty()) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(buffer[0]));
        }
        if (dataType == "INT16") {
            if (buffer.size() < 2) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int16_t>(readU16BE(buffer))));
        }
        if (dataType == "UINT16") {
            if (buffer.size() < 2) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(readU16BE(buffer)));
        }
        if (dataType == "INT32") {
            if (buffer.size() < 4) return Json::nullValue;
            return Json::Value(static_cast<Json::Int64>(static_cast<int32_t>(readU32BE(buffer))));
        }
        if (dataType == "UINT32") {
            if (buffer.size() < 4) return Json::nullValue;
            return Json::Value(static_cast<Json::UInt64>(readU32BE(buffer)));
        }
        if (dataType == "FLOAT") {
            if (buffer.size() < 4) return Json::nullValue;
            float value = 0.0f;
            uint32_t raw = readU32BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "LREAL") {
            if (buffer.size() < 8) return Json::nullValue;
            double value = 0.0;
            uint64_t raw = readU64BE(buffer);
            std::memcpy(&value, &raw, sizeof(value));
            return Json::Value(value);
        }
        if (dataType == "STRING") {
            std::string value(reinterpret_cast<const char*>(buffer.data()), buffer.size());
            size_t zero = value.find('\0');
            if (zero != std::string::npos) {
                value.resize(zero);
            }
            return Json::Value(sanitizeUtf8(value));
        }
        return Json::Value(bytesToHex(buffer));
    }

    static Json::Value buildReadElement(const S7AreaDefinition& area,
                                        const std::vector<uint8_t>& buffer) {
        Json::Value element(Json::objectValue);
        const int dbNumber = resolvedDbNumber(area);
        element["name"] = area.name;
        element["value"] = decodeAreaValue(area, buffer);
        element["hex"] = bytesToHex(buffer);
        element["area"] = area.area;
        element["dataType"] = normalizeDataType(area.dataType);
        element["dbNumber"] = dbNumber;
        element["start"] = area.start;
        element["size"] = area.size;
        if (normalizeDataType(area.dataType) == "BOOL") {
            element["startBit"] = area.startBit;
        }
        if (area.writable) {
            element["writable"] = true;
        }
        if (!area.remark.empty()) {
            element["remark"] = area.remark;
        }
        return element;
    }

    static ParsedFrameResult buildPollReadResult(int deviceId,
                                                 int linkId,
                                                 Json::Value data,
                                                 const std::string& reportTime) {
        ParsedFrameResult result;
        result.deviceId = deviceId;
        result.linkId = linkId;
        result.protocol = Constants::PROTOCOL_S7;
        result.funcCode = FUNC_READ;
        result.reportTime = reportTime.empty() ? makeUtcNowString() : reportTime;

        Json::Value payload(Json::objectValue);
        payload["funcCode"] = FUNC_READ;
        payload["funcName"] = "S7采集";
        payload["direction"] = "UP";
        payload["data"] = std::move(data);
        result.data = std::move(payload);
        return result;
    }

    static std::vector<uint8_t> parseHexBytes(std::string hex) {
        std::vector<uint8_t> bytes;
        if (hex.empty()) {
            return bytes;
        }
        if (hex.size() % 2 != 0) {
            hex = "0" + hex;
        }
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto byte = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            bytes.push_back(byte);
        }
        return bytes;
    }

    static std::string jsonValueToString(const Json::Value& value) {
        if (value.isString()) {
            return value.asString();
        }
        if (value.isBool()) {
            return value.asBool() ? "1" : "0";
        }
        if (value.isInt64()) {
            return std::to_string(value.asInt64());
        }
        if (value.isUInt64()) {
            return std::to_string(value.asUInt64());
        }
        if (value.isDouble()) {
            std::ostringstream oss;
            oss << value.asDouble();
            return oss.str();
        }
        return "";
    }

    static std::string trimCopy(std::string value) {
        auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) {
            return !isSpace(ch);
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) {
            return !isSpace(ch);
        }).base(), value.end());
        return value;
    }

    static bool parseBoolValue(std::string value, bool& result) {
        value = trimCopy(std::move(value));
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (value == "1" || value == "TRUE" || value == "ON") {
            result = true;
            return true;
        }
        if (value == "0" || value == "FALSE" || value == "OFF") {
            result = false;
            return true;
        }
        return false;
    }

    static std::optional<std::vector<uint8_t>> encodeAreaValue(
        const S7AreaDefinition& area,
        const Json::Value& element,
        const std::vector<uint8_t>* existingBytes = nullptr
    ) {
        const std::string valueHex = element.get("valueHex", "").asString();
        if (!valueHex.empty()) {
            return parseHexBytes(valueHex);
        }

        const std::string rawValue = trimCopy(jsonValueToString(element["value"]));
        if (rawValue.empty()) {
            return std::nullopt;
        }

        const std::string dataType = normalizeDataType(area.dataType);

        try {
            if (dataType == "BOOL") {
                bool boolValue = false;
                if (!parseBoolValue(rawValue, boolValue)) {
                    return std::nullopt;
                }
                std::vector<uint8_t> bytes;
                if (existingBytes && !existingBytes->empty()) {
                    bytes = *existingBytes;
                } else {
                    bytes.assign(static_cast<std::size_t>(std::max(1, area.size)), 0);
                }
                if (bytes.empty()) {
                    bytes.push_back(0);
                }
                const int bit = std::clamp(area.startBit, 0, 7);
                if (boolValue) {
                    bytes[0] = static_cast<uint8_t>(bytes[0] | (1u << bit));
                } else {
                    bytes[0] = static_cast<uint8_t>(bytes[0] & ~(1u << bit));
                }
                return bytes;
            }

            if (dataType == "INT8") {
                return std::vector<uint8_t>{static_cast<uint8_t>(static_cast<int8_t>(std::stoi(rawValue)))};
            }
            if (dataType == "UINT8") {
                return std::vector<uint8_t>{static_cast<uint8_t>(std::stoul(rawValue))};
            }
            if (dataType == "INT16" || dataType == "UINT16"
                || area.area == "CT" || area.area == "TM") {
                std::vector<uint8_t> bytes;
                appendU16BE(bytes, static_cast<uint16_t>(
                    dataType == "INT16"
                        ? static_cast<int16_t>(std::stoi(rawValue))
                        : static_cast<uint16_t>(std::stoul(rawValue))));
                return bytes;
            }
            if (dataType == "INT32") {
                std::vector<uint8_t> bytes;
                appendU32BE(bytes, static_cast<uint32_t>(static_cast<int32_t>(std::stoll(rawValue))));
                return bytes;
            }
            if (dataType == "UINT32") {
                std::vector<uint8_t> bytes;
                appendU32BE(bytes, static_cast<uint32_t>(std::stoull(rawValue)));
                return bytes;
            }
            if (dataType == "FLOAT") {
                float value = std::stof(rawValue);
                uint32_t raw = 0;
                std::memcpy(&raw, &value, sizeof(value));
                std::vector<uint8_t> bytes;
                appendU32BE(bytes, raw);
                return bytes;
            }
            if (dataType == "LREAL") {
                double value = std::stod(rawValue);
                uint64_t raw = 0;
                std::memcpy(&raw, &value, sizeof(value));
                std::vector<uint8_t> bytes;
                appendU64BE(bytes, raw);
                return bytes;
            }
            if (dataType == "STRING") {
                std::vector<uint8_t> bytes(
                    static_cast<std::size_t>(std::max(1, area.size)), 0);
                const std::size_t copySize = std::min(bytes.size(), rawValue.size());
                std::memcpy(bytes.data(), rawValue.data(), copySize);
                return bytes;
            }

            return parseHexBytes(rawValue);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    static const S7AreaDefinition* findAreaDefinition(const S7DeviceRuntime& runtime,
                                                      const Json::Value& element) {
        const std::string elementId = element.get("elementId", "").asString();
        if (!elementId.empty()) {
            for (const auto& area : runtime.areas) {
                if (area.id == elementId) {
                    return &area;
                }
            }
        }

        const std::string areaName = toUpper(element.get("area", "").asString());
        const int start = element.get("start", -1).asInt();
        int dbNumber = element.get("dbNumber", -1).asInt();
        if (areaName == "V" && dbNumber <= 0) {
            dbNumber = 1;
        }
        if (areaName.empty() || start < 0) {
            return nullptr;
        }

        for (const auto& area : runtime.areas) {
            if (area.area != areaName || area.start != start) {
                continue;
            }
            if (resolvedDbNumber(area) != dbNumber) {
                continue;
            }
            return &area;
        }
        return nullptr;
    }

    static S7ConnectionConfig parseConnection(const Json::Value& config, const std::string& plcModel) {
        S7ConnectionConfig connection;
        const auto preset = resolvePreset(plcModel).value_or(S7ConnectionPreset{});
        const bool isS7_200 = toUpper(plcModel) == "S7-200";
        connection.mode = preset.mode;
        connection.rack = preset.rack;
        connection.slot = preset.slot;
        connection.localTSAP = preset.localTSAP;
        connection.remoteTSAP = preset.remoteTSAP;

        const bool hasNestedConnection = config.isMember("connection") && config["connection"].isObject();
        if (hasNestedConnection) {
            const auto& conn = config["connection"];
            connection.host = conn.get("host", "").asString();
            connection.connectionType = toUpper(conn.get("connectionType", "PG").asString());
            if (isS7_200) {
                connection.mode = "TSAP";
            } else if (conn.isMember("mode")) {
                connection.mode = normalizeConnectionMode(conn.get("mode", "").asString());
            } else if (conn.isMember("localTSAP") || conn.isMember("remoteTSAP")) {
                connection.mode = "TSAP";
            }
            connection.rack = conn.get("rack", connection.rack).asInt();
            connection.slot = conn.get("slot", connection.slot).asInt();
            if (auto localTSAP = parseTsapHex(conn["localTSAP"])) {
                connection.localTSAP = *localTSAP;
            }
            if (auto remoteTSAP = parseTsapHex(conn["remoteTSAP"])) {
                connection.remoteTSAP = *remoteTSAP;
            }
            connection.pingTimeoutMs = conn.get("pingTimeout", connection.pingTimeoutMs).asInt();
            connection.sendTimeoutMs = conn.get("sendTimeout", connection.sendTimeoutMs).asInt();
            connection.recvTimeoutMs = conn.get("recvTimeout", connection.recvTimeoutMs).asInt();
            connection.retryDelayMs = conn.get("retryDelay", connection.retryDelayMs).asInt();
            if (conn.isMember("pduRequestLength")) {
                connection.pduRequestLength = static_cast<std::uint16_t>(
                    std::clamp(conn.get("pduRequestLength", static_cast<int>(connection.pduRequestLength)).asInt(),
                        240,
                        static_cast<int>(kDefaultIsoPduSize - 7)));
            } else if (conn.isMember("pduLength")) {
                connection.pduRequestLength = static_cast<std::uint16_t>(
                    std::clamp(conn.get("pduLength", static_cast<int>(connection.pduRequestLength)).asInt(),
                        240,
                        static_cast<int>(kDefaultIsoPduSize - 7)));
            }
            if (connection.pingTimeoutMs <= 0) connection.pingTimeoutMs = kDefaultS7RecvTimeoutMs;
            if (connection.sendTimeoutMs <= 0) connection.sendTimeoutMs = kDefaultS7SendTimeoutMs;
            if (connection.recvTimeoutMs <= 0) connection.recvTimeoutMs = kDefaultS7RecvTimeoutMs;
            if (connection.retryDelayMs < 0) connection.retryDelayMs = 1000;
            if (connection.recvTimeoutMs < connection.sendTimeoutMs) {
                connection.recvTimeoutMs = connection.sendTimeoutMs;
            }
            if (connection.pingTimeoutMs < connection.recvTimeoutMs) {
                connection.pingTimeoutMs = connection.recvTimeoutMs;
            }
        }
        if (!hasNestedConnection) {
            const Json::Value* compatConn = &config;
            connection.host = compatConn->get("host", connection.host).asString();
            connection.connectionType = toUpper(compatConn->get("connectionType", connection.connectionType).asString());
            if (compatConn->isMember("mode")) {
                connection.mode = normalizeConnectionMode(compatConn->get("mode", connection.mode).asString());
            } else if (compatConn->isMember("localTSAP") || compatConn->isMember("remoteTSAP")) {
                connection.mode = "TSAP";
            }
            connection.rack = compatConn->get("rack", connection.rack).asInt();
            connection.slot = compatConn->get("slot", connection.slot).asInt();
            if (auto localTSAP = parseTsapHex((*compatConn)["localTSAP"])) {
                connection.localTSAP = *localTSAP;
            }
            if (auto remoteTSAP = parseTsapHex((*compatConn)["remoteTSAP"])) {
                connection.remoteTSAP = *remoteTSAP;
            }
            if (compatConn->isMember("pduRequestLength")) {
                connection.pduRequestLength = static_cast<std::uint16_t>(
                    std::clamp(compatConn->get("pduRequestLength", static_cast<int>(connection.pduRequestLength)).asInt(),
                        240,
                        static_cast<int>(kDefaultIsoPduSize - 7)));
            } else if (compatConn->isMember("pduLength")) {
                connection.pduRequestLength = static_cast<std::uint16_t>(
                    std::clamp(compatConn->get("pduLength", static_cast<int>(connection.pduRequestLength)).asInt(),
                        240,
                        static_cast<int>(kDefaultIsoPduSize - 7)));
            }
        }
        if (isS7_200) {
            connection.mode = "TSAP";
        }
        connection.pollIntervalSec = config.get("pollInterval", 5).asInt();
        if (connection.pollIntervalSec < 1) connection.pollIntervalSec = 1;
        return connection;
    }

    static std::vector<S7AreaDefinition> parseAreas(const Json::Value& config) {
        const Json::Value* areas = nullptr;
        if (config.isMember("areas") && config["areas"].isArray()) {
            areas = &config["areas"];
        } else if (config.isMember("poll") && config["poll"].isObject() && config["poll"].isMember("areas") && config["poll"]["areas"].isArray()) {
            areas = &config["poll"]["areas"];
        }

        std::vector<S7AreaDefinition> result;
        if (!areas) return result;
        const std::string plcModel = toUpper(config.get("plcModel", "").asString());
        const bool isS7_200 = plcModel == "S7-200";
        for (const auto& area : *areas) {
            if (!area.isObject()) continue;
            S7AreaDefinition def;
            const std::string areaType = toUpper(area.get("area", "DB").asString());
            def.id = area.get("id", "").asString();
            def.name = area.get("name", def.id).asString();
            def.area = areaType;
            def.dataType = normalizeDataType(area.get("dataType", (areaType == "CT" || areaType == "TM") ? "UINT16" : "INT16").asString());
            if (areaType == "CT" || areaType == "TM") {
                def.dataType = "UINT16";
            }
            def.dbNumber = (areaType == "V" || (isS7_200 && areaType == "DB")) ? 1 : area.get("dbNumber", 0).asInt();
            def.start = area.get("start", 0).asInt();
            def.startBit = std::clamp(area.get("startBit", 0).asInt(), 0, 7);
            def.size = std::max(1, area.get("size", 1).asInt());
            def.writable = area.get("writable", false).asBool();
            def.remark = area.get("remark", "").asString();
            if (!def.id.empty()) {
                result.push_back(std::move(def));
            }
        }
        return result;
    }

    static std::shared_ptr<S7DeviceRuntime> buildRuntime(const DeviceCache::CachedDevice& device) {
        if (device.protocolType != Constants::PROTOCOL_S7) {
            return nullptr;
        }
        auto runtime = std::make_shared<S7DeviceRuntime>();
        runtime->deviceId = device.id;
        runtime->linkId = device.linkId;
        runtime->deviceName = device.name.empty()
            ? (device.linkName.empty() ? ("S7-" + std::to_string(device.id)) : device.linkName)
            : device.name;
        runtime->deviceCode = device.deviceCode.empty() ? ("s7_" + std::to_string(device.id)) : device.deviceCode;
        runtime->tcpServerMode = device.linkMode == Constants::LINK_MODE_TCP_SERVER;
        if (runtime->tcpServerMode) {
        runtime->sessionKey = std::to_string(device.linkId) + ":"
                + detail::makeRegistrationToken(device.registrationBytes);
        }
        const std::string plcModel = device.protocolConfig.get("plcModel", "").asString();
        runtime->connection = parseConnection(device.protocolConfig, plcModel);
        if (device.readInterval > 0) {
            runtime->connection.pollIntervalSec = std::clamp(device.readInterval, 1, 3600);
        }
        if (runtime->tcpServerMode) {
            runtime->connection.host = "tcpserver";
        } else if (!device.linkIp.empty()) {
            runtime->connection.host = device.linkIp;
        }
        if (runtime->connection.host.empty()) {
            return nullptr;
        }
        runtime->areas = parseAreas(device.protocolConfig);
        return runtime;
    }

    static std::string deviceLabel(const S7DeviceRuntime& runtime) {
        return runtime.deviceName.empty()
            ? ("S7-" + std::to_string(runtime.deviceId))
            : runtime.deviceName;
    }

    static std::uintptr_t deviceQueueKey(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        return reinterpret_cast<std::uintptr_t>(runtime.get());
    }

    static std::uintptr_t discoveryQueueKey(int linkId) {
        constexpr std::uintptr_t tag = static_cast<std::uintptr_t>(1)
            << (sizeof(std::uintptr_t) * 8 - 1);
        return tag | static_cast<std::uintptr_t>(static_cast<std::uint32_t>(linkId));
    }

    static std::uintptr_t runtimeQueueKey(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        if (!runtime) {
            return 0;
        }

        std::lock_guard runtimeLock(runtime->mutex);
        const bool sessionReady = runtime->sessionBound
            && runtime->sessionLinkId > 0
            && !runtime->sessionClientAddr.empty();
        if (runtime->tcpServerMode && !sessionReady) {
            return discoveryQueueKey(runtime->linkId);
        }
        return deviceQueueKey(runtime);
    }

    void triggerLinkDiscoveryNow(int linkId) {
        if (!pollScheduler_ || linkId <= 0) {
            return;
        }

        auto runtimes = snapshotRuntimesLocked();
        for (const auto& runtime : runtimes) {
            bool shouldTrigger = false;
            int deviceId = 0;
            {
                std::lock_guard runtimeLock(runtime->mutex);
                deviceId = runtime->deviceId;
                shouldTrigger = runtime->tcpServerMode
                    && runtime->linkId == linkId
                    && !runtime->sessionBound;
            }
            if (shouldTrigger && deviceId > 0) {
                pollScheduler_->triggerNow(deviceId);
            }
        }
    }

    void syncActiveServerSessionsFromTransport() {
        if (!sessionRegistry_ || !sessionManager_) {
            return;
        }

        auto definitions = sessionRegistry_->getAllDefinitions();
        std::set<int> linkIds;
        for (const auto& definition : definitions) {
            if (definition.linkId > 0) {
                linkIds.insert(definition.linkId);
            }
        }

        for (int linkId : linkIds) {
            auto status = LinkTransportFacade::instance().getStatus(linkId);
            const auto& clients = status["clients"];
            if (!clients.isArray()) {
                continue;
            }

            int synced = 0;
            for (const auto& client : clients) {
                const std::string clientAddr = client.asString();
                if (clientAddr.empty()) {
                    continue;
                }
                sessionManager_->onConnected(linkId, clientAddr);
                ++synced;
            }
            if (synced > 0) {
                LOG_INFO << "[S7][Adapter] Synced active TCP server session(s): linkId="
                         << linkId << ", count=" << synced;
            }
        }
    }

    void reconcileSessionBindingsAfterRegistryReload() {
        if (!sessionManager_ || !sessionRegistry_) {
            return;
        }

        const auto staleSessions = sessionManager_->reconcileDefinitions(sessionRegistry_->getAllDefinitions());
        for (const auto& session : staleSessions) {
            if (session.linkId > 0 && !session.clientAddr.empty()) {
                LinkTransportFacade::instance().disconnectServerClient(session.linkId, session.clientAddr);
            }
            if (session.deviceId > 0) {
                closeRuntimeClient(session.deviceId);
            } else if (session.probingDeviceId > 0) {
                closeRuntimeClient(session.probingDeviceId);
            }
        }
    }

    void restoreRuntimeSessionBindings() {
        if (!sessionManager_) {
            return;
        }

        const auto sessions = sessionManager_->listSessions();
        for (const auto& session : sessions) {
            if (session.bindState != SessionBindState::Bound || session.deviceId <= 0) {
                continue;
            }
            attachSessionBinding(session.deviceId, session.linkId, session.clientAddr, session.dtuKey);
        }
    }

    /**
     * @brief 从 DeviceCache 重建运行时目录和轮询配置
     */
    void buildRuntimeCatalog(
        const std::vector<DeviceCache::CachedDevice>& devices,
        std::unordered_map<int, std::shared_ptr<S7DeviceRuntime>>& next,
        std::vector<S7PollScheduler::DeviceConfig>& pollConfigs,
        std::size_t& sessionCount
    ) {
        next.clear();
        pollConfigs.clear();
        sessionCount = 0;

        for (const auto& device : devices) {
            if (device.protocolType != Constants::PROTOCOL_S7) {
                continue;
            }

            auto runtime = buildRuntime(device);
            if (!runtime) {
                continue;
            }

            if (auto existing = findRuntimeLocked(runtime->deviceId)) {
                bool resetClient = false;
                {
                    std::scoped_lock runtimeLock(existing->clientMutex, existing->mutex);
                    resetClient = existing->linkId != runtime->linkId
                        || existing->tcpServerMode != runtime->tcpServerMode
                        || existing->sessionKey != runtime->sessionKey
                        || !sameS7Connection(existing->connection, runtime->connection);

                    if (resetClient) {
                        ++existing->connectGeneration;
                        existing->connectInProgress = false;
                        existing->sessionBound = false;
                        existing->sessionDiscoveryInFlight = false;
                        existing->sessionDiscoveryStartedAt = {};
                        existing->sessionLinkId = 0;
                        existing->sessionClientAddr.clear();
                        existing->resetClient();
                    }

                    existing->linkId = runtime->linkId;
                    existing->deviceName = std::move(runtime->deviceName);
                    existing->deviceCode = std::move(runtime->deviceCode);
                    existing->sessionKey = std::move(runtime->sessionKey);
                    existing->connection = std::move(runtime->connection);
                    existing->areas = std::move(runtime->areas);
                    existing->tcpServerMode = runtime->tcpServerMode;
                    runtime = existing;
                }
            }

            if (runtime->tcpServerMode) {
                ++sessionCount;
            }

            if (!runtime->areas.empty()) {
                pollConfigs.push_back(S7PollScheduler::DeviceConfig{
                    .deviceId = runtime->deviceId,
                    .deviceName = runtime->deviceName,
                    .readIntervalSec = runtime->connection.pollIntervalSec,
                    .fastReadDurationSec = device.commandFastReadDuration,
                    .fastReadIntervalSec = device.commandFastReadInterval,
                    .enabled = true
                });
            }

            next.emplace(runtime->deviceId, std::move(runtime));
        }
    }

    /**
     * @brief 刷新 S7 运行态
     *
     * 同步当前 TCP 连接并保留仍然匹配的 session binding，再按最新配置重建 runtime 和轮询表。
     */
    Task<> refreshDevices() {
        if (sessionRegistry_) {
            co_await sessionRegistry_->reload();
        }

        syncActiveServerSessionsFromTransport();

        auto devices = co_await DeviceCache::instance().getDevices();
        std::unordered_map<int, std::shared_ptr<S7DeviceRuntime>> next;
        std::size_t sessionCount = 0;
        std::vector<S7PollScheduler::DeviceConfig> pollConfigs;
        buildRuntimeCatalog(devices, next, pollConfigs, sessionCount);

        std::size_t deviceCount = 0;
        {
            std::lock_guard lock(devicesMutex_);
            devices_ = std::move(next);
            deviceCount = devices_.size();
        }
        reconcileSessionBindingsAfterRegistryReload();
        restoreRuntimeSessionBindings();
        LOG_INFO << "[S7][Adapter] Reloaded " << deviceCount
                 << " S7 device(s), tcp server devices=" << sessionCount;
        if (pollScheduler_) {
            pollScheduler_->reload(pollConfigs);
        }
        co_return;
    }

    std::shared_ptr<S7DeviceRuntime> findRuntimeLocked(int deviceId) {
        std::lock_guard lock(devicesMutex_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end()) {
            return nullptr;
        }
        return it->second;
    }

    std::vector<std::shared_ptr<S7DeviceRuntime>> snapshotRuntimesLocked() const {
        std::vector<std::shared_ptr<S7DeviceRuntime>> runtimes;
        std::lock_guard lock(devicesMutex_);
        runtimes.reserve(devices_.size());
        for (const auto& [_, runtime] : devices_) {
            runtimes.push_back(runtime);
        }
        return runtimes;
    }

    static std::string bytesToString(const std::vector<uint8_t>& bytes) {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    static void applyConnectionTimeouts(Client& client, const S7ConnectionConfig& connection) {
        const auto setTimeout = [&](int param, int value, const char* name) {
            if (value <= 0) {
                return;
            }
            int32_t timeout = value;
            if (client.setParam(param, &timeout) != 0) {
                LOG_DEBUG << "[S7][Adapter] Failed to set " << name
                          << " for host=" << connection.host
                          << ", value=" << value;
            }
        };

        setTimeout(p_i32_PingTimeout, connection.pingTimeoutMs, "PingTimeout");
        setTimeout(p_i32_SendTimeout, connection.sendTimeoutMs, "SendTimeout");
        setTimeout(p_i32_RecvTimeout, connection.recvTimeoutMs, "RecvTimeout");
        std::uint16_t pduRequestLength = connection.pduRequestLength;
        if (client.setParam(p_u16_PduRequest, &pduRequestLength) != 0) {
            LOG_DEBUG << "[S7][Adapter] Failed to set PduRequestLength"
                      << " for host=" << connection.host
                      << ", value=" << pduRequestLength;
        }
    }

    static bool isSessionReadyLocked(const S7DeviceRuntime& runtime) {
        return runtime.tcpServerMode
            && runtime.connected
            && !runtime.connectInProgress
            && runtime.sessionBound
            && !runtime.sessionClientAddr.empty()
            && runtime.sessionLinkId > 0;
    }

    std::vector<S7DtuSession> acquireLinkDiscoverySessions(int linkId, int deviceId) {
        if (!sessionManager_ || linkId <= 0 || deviceId <= 0) {
            return {};
        }

        auto probingSession = sessionManager_->getProbingSessionByDevice(deviceId);
        if (probingSession
            && probingSession->linkId == linkId
            && !probingSession->clientAddr.empty()) {
            return sessionManager_->acquireLinkProbeSessions(linkId, deviceId);
        }
        return sessionManager_->acquireLinkProbeSessions(linkId, deviceId);
    }

    void clearDiscoveryState(const std::shared_ptr<S7DeviceRuntime>& runtime, bool advanceCursor) {
        if (!runtime) {
            return;
        }

        int deviceId = 0;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            deviceId = runtime->deviceId;
            runtime->sessionDiscoveryInFlight = false;
            runtime->sessionDiscoveryStartedAt = {};
        }

        if (sessionManager_ && deviceId > 0) {
            sessionManager_->releaseProbingSession(deviceId, advanceCursor);
        }
    }

    void attachSessionBinding(int deviceId, int linkId, const std::string& clientAddr, const std::string& sessionKey) {
        bool shouldLog = false;
        std::string deviceName;
        {
            auto runtime = findRuntimeLocked(deviceId);
            if (!runtime || !runtime->tcpServerMode) {
                return;
            }

            std::lock_guard runtimeLock(runtime->mutex);
            deviceName = runtime->deviceName;
            shouldLog = !runtime->sessionBound
                || runtime->sessionLinkId != linkId
                || runtime->sessionClientAddr != clientAddr
                || (!sessionKey.empty() && runtime->sessionKey != sessionKey);
            runtime->sessionBound = true;
            runtime->sessionDiscoveryInFlight = false;
            runtime->sessionDiscoveryStartedAt = {};
            runtime->sessionLinkId = linkId;
            runtime->sessionClientAddr = clientAddr;
            if (!sessionKey.empty()) {
                runtime->sessionKey = sessionKey;
            }
        }

        if (shouldLog) {
            LOG_INFO << "[S7][Adapter] Session bound: " << (deviceName.empty() ? "S7-unknown" : deviceName)
                     << "(id=" << deviceId << ")"
                     << ", linkId=" << linkId
                     << ", client=" << clientAddr
                     << ", sessionKey=" << (sessionKey.empty() ? "<unknown>" : sessionKey);
        }

        if (shouldLog && pollScheduler_) {
            pollScheduler_->triggerNow(deviceId);
        }
    }

    void closeRuntimeClient(int deviceId) {
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime) {
            return;
        }

        {
            std::lock_guard runtimeLock(runtime->mutex);
            LOG_INFO << "[S7][Adapter] Closing session runtime: " << deviceLabel(*runtime)
                     << "(id=" << deviceId << ")"
                     << ", client=" << (runtime->sessionClientAddr.empty() ? "<unbound>" : runtime->sessionClientAddr);
            runtime->sessionBound = false;
            runtime->sessionDiscoveryInFlight = false;
            runtime->connectInProgress = false;
            ++runtime->connectGeneration;
            runtime->sessionDiscoveryStartedAt = {};
            runtime->sessionLinkId = 0;
            runtime->sessionClientAddr.clear();
            runtime->connected = false;
        }

        {
            std::lock_guard clientLock(runtime->clientMutex);
            runtime->resetClient();
        }
    }

    static std::vector<std::uint8_t> wrapS7Payload(const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> frame;
        frame.reserve(7 + payload.size());
        frame.push_back(kIsoTcpVersion);
        frame.push_back(0x00);
        appendBe16(frame, static_cast<std::uint16_t>(7 + payload.size()));
        frame.push_back(kCotpDtLength);
        frame.push_back(kCotpDt);
        frame.push_back(kCotpEot);
        frame.insert(frame.end(), payload.begin(), payload.end());
        return frame;
    }

    void completeAsyncExchange(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<S7DeviceRuntime::AsyncExchange>& exchange,
        int rc,
        std::vector<std::uint8_t> payload) {

        if (!runtime || !exchange) {
            return;
        }

        std::function<void(int, std::vector<std::uint8_t>)> complete;
        {
            std::lock_guard lock(runtime->mutex);
            if (exchange->done || runtime->asyncExchange != exchange) {
                return;
            }
            exchange->done = true;
            complete = std::move(exchange->complete);
            runtime->asyncExchange.reset();
        }
        if (complete) {
            complete(rc, std::move(payload));
        }
    }

    bool enqueueDeviceOperation(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        ProtocolJobPriority priority,
        std::function<void(std::function<void()>)> run) {
        if (!runtime || !run) {
            return false;
        }

        bool shouldStart = false;
        {
            std::lock_guard lock(runtime->mutex);
            if (!runtime->operationQueue.push(
                    S7DeviceRuntime::QueuedOperation{.priority = priority, .run = std::move(run)},
                    priority)) {
                LOG_WARN << "[S7][Adapter] Device operation queue full: "
                         << deviceLabel(*runtime) << "(id=" << runtime->deviceId << ")"
                         << ", high=" << runtime->operationQueue.highSize()
                         << ", normal=" << runtime->operationQueue.normalSize();
                return false;
            }
            shouldStart = !runtime->operationRunning;
        }

        if (shouldStart) {
            startNextDeviceOperation(runtime);
        }
        return true;
    }

    void startNextDeviceOperation(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        if (!runtime) {
            return;
        }

        S7DeviceRuntime::QueuedOperation op;
        {
            std::lock_guard lock(runtime->mutex);
            if (runtime->operationRunning) {
                return;
            }
            if (!runtime->operationQueue.popNext(op)) {
                return;
            }
            runtime->operationRunning = true;
        }

        op.run([this, runtime]() {
            finishDeviceOperation(runtime);
        });
    }

    void finishDeviceOperation(const std::shared_ptr<S7DeviceRuntime>& runtime) {
        if (!runtime) {
            return;
        }

        bool hasNext = false;
        {
            std::lock_guard lock(runtime->mutex);
            runtime->operationRunning = false;
            hasNext = !runtime->operationQueue.empty();
        }
        if (hasNext) {
            startNextDeviceOperation(runtime);
        }
    }

    bool enqueueLinkOperation(
        int linkId,
        std::function<void(std::function<void()>)> run) {

        if (linkId <= 0 || !run) {
            return false;
        }

        bool shouldStart = false;
        {
            std::lock_guard lock(linkOperationMutex_);
            auto& runtime = linkOperationQueues_[linkId];
            if (!runtime.queue.push(LinkQueuedOperation{.run = std::move(run)})) {
                LOG_WARN << "[S7][Adapter] Link broadcast queue full: linkId=" << linkId
                         << ", high=" << runtime.queue.highSize()
                         << ", normal=" << runtime.queue.normalSize();
                return false;
            }
            shouldStart = !runtime.running;
        }

        if (shouldStart) {
            startNextLinkOperation(linkId);
        }
        return true;
    }

    void startNextLinkOperation(int linkId) {
        LinkQueuedOperation op;
        {
            std::lock_guard lock(linkOperationMutex_);
            auto it = linkOperationQueues_.find(linkId);
            if (it == linkOperationQueues_.end() || it->second.running) {
                return;
            }
            if (!it->second.queue.popNext(op)) {
                return;
            }
            it->second.running = true;
        }

        op.run([this, linkId]() {
            finishLinkOperation(linkId);
        });
    }

    void finishLinkOperation(int linkId) {
        bool hasNext = false;
        {
            std::lock_guard lock(linkOperationMutex_);
            auto it = linkOperationQueues_.find(linkId);
            if (it == linkOperationQueues_.end()) {
                return;
            }
            it->second.running = false;
            hasNext = !it->second.queue.empty();
            if (!hasNext) {
                linkOperationQueues_.erase(it);
            }
        }
        if (hasNext) {
            startNextLinkOperation(linkId);
        }
    }

    bool sendAsyncSessionFrame(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::vector<uint8_t>& frame,
        bool allowBroadcast) {

        if (!runtime || frame.empty()) {
            return false;
        }

        int deviceId = 0;
        int requestLinkId = 0;
        bool sessionBound = false;
        int sessionLinkId = 0;
        std::string sessionClientAddr;
        {
            std::lock_guard lock(runtime->mutex);
            deviceId = runtime->deviceId;
            requestLinkId = runtime->linkId;
            sessionBound = runtime->sessionBound
                && runtime->sessionLinkId > 0
                && !runtime->sessionClientAddr.empty();
            sessionLinkId = runtime->sessionLinkId;
            sessionClientAddr = runtime->sessionClientAddr;
        }

        const std::string payload = bytesToString(frame);
        if (sessionBound) {
            if (LinkTransportFacade::instance().sendToClient(sessionLinkId, sessionClientAddr, payload)) {
                return true;
            }
            LinkTransportFacade::instance().forceDisconnectServerClient(sessionLinkId, sessionClientAddr);
            return false;
        }

        if (!allowBroadcast) {
            return false;
        }

        auto probingSessions = acquireLinkDiscoverySessions(requestLinkId, deviceId);
        if (probingSessions.empty()) {
            return false;
        }

        {
            std::lock_guard lock(runtime->mutex);
            runtime->sessionDiscoveryInFlight = true;
            runtime->sessionDiscoveryStartedAt = std::chrono::steady_clock::now();
        }

        std::size_t sentCount = 0;
        std::size_t failedCount = 0;
        for (const auto& probingSession : probingSessions) {
            if (probingSession.clientAddr.empty() || probingSession.linkId <= 0) {
                continue;
            }
            LOG_DEBUG << "[S7][Adapter] TX async probe to session linkId=" << probingSession.linkId
                      << ", client=" << probingSession.clientAddr
                      << ", device=" << deviceLabel(*runtime) << "(id=" << deviceId << ")"
                      << ", bytes=" << frame.size()
                      << ", hex=" << bytesToHex(frame);
            if (LinkTransportFacade::instance().sendToClient(
                    probingSession.linkId, probingSession.clientAddr, payload)) {
                ++sentCount;
                continue;
            }
            ++failedCount;
            LinkTransportFacade::instance().forceDisconnectServerClient(
                probingSession.linkId, probingSession.clientAddr);
        }

        if (sentCount > 0) {
            LOG_INFO << "[S7][Adapter] Broadcast discovery probe: " << deviceLabel(*runtime)
                     << "(id=" << deviceId << ")"
                     << ", linkId=" << requestLinkId
                     << ", sent=" << sentCount
                     << ", failed=" << failedCount
                     << ", bytes=" << frame.size()
                     << ", async=yes";
            return true;
        }
        return false;
    }

    void completeAsyncConnect(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<S7DeviceRuntime::AsyncConnect>& connect,
        bool success) {

        if (!runtime || !connect) {
            return;
        }

        std::function<void(bool)> complete;
        int deviceId = 0;
        std::string label;
        {
            std::lock_guard lock(runtime->mutex);
            if (connect->done || runtime->asyncConnect != connect) {
                return;
            }
            connect->done = true;
            complete = std::move(connect->complete);
            runtime->asyncConnect.reset();
            runtime->connectInProgress = false;
            runtime->connected = success;
            runtime->lastConnectAttempt = std::chrono::steady_clock::now();
            if (!success) {
                runtime->sessionDiscoveryInFlight = false;
                runtime->sessionDiscoveryStartedAt = {};
            }
            deviceId = runtime->deviceId;
            label = deviceLabel(*runtime);
        }

        if (!success && sessionManager_ && deviceId > 0) {
            sessionManager_->releaseProbingSession(deviceId, true);
        }
        LOG_INFO << "[S7][Adapter] Async connect " << (success ? "ready" : "failed")
                 << ": " << label << "(id=" << deviceId << ")";
        if (complete) {
            complete(success);
        }
    }

    void processAsyncConnectBytes(const std::shared_ptr<S7DeviceRuntime>& runtime, std::vector<uint8_t> bytes) {
        if (!runtime || bytes.empty()) {
            return;
        }

        std::vector<std::pair<std::shared_ptr<S7DeviceRuntime::AsyncConnect>, std::vector<uint8_t>>> frames;
        {
            std::lock_guard lock(runtime->mutex);
            auto connect = runtime->asyncConnect;
            if (!connect) {
                return;
            }
            connect->rxBuffer.insert(connect->rxBuffer.end(), bytes.begin(), bytes.end());
            while (connect->rxBuffer.size() >= 4) {
                if (connect->rxBuffer[0] != kIsoTcpVersion) {
                    frames.emplace_back(connect, std::vector<uint8_t>{});
                    connect->rxBuffer.clear();
                    break;
                }
                const std::uint16_t totalLength = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(connect->rxBuffer[2]) << 8)
                    | connect->rxBuffer[3]);
                if (totalLength < 7) {
                    frames.emplace_back(connect, std::vector<uint8_t>{});
                    connect->rxBuffer.clear();
                    break;
                }
                if (connect->rxBuffer.size() < totalLength) {
                    break;
                }

                std::vector<uint8_t> frame;
                frame.reserve(totalLength);
                for (std::uint16_t i = 0; i < totalLength; ++i) {
                    frame.push_back(connect->rxBuffer.front());
                    connect->rxBuffer.pop_front();
                }
                frames.emplace_back(connect, std::move(frame));
            }
        }

        for (auto& [connect, frame] : frames) {
            if (frame.empty()) {
                completeAsyncConnect(runtime, connect, false);
                continue;
            }

            if (connect->stage == S7DeviceRuntime::AsyncConnect::Stage::IsoConfirm) {
                int rc = kS7ErrInvalidHandle;
                std::vector<uint8_t> setupFrame;
                {
                    std::lock_guard clientLock(runtime->clientMutex);
                    if (runtime->client) {
                        rc = runtime->client->parseConnectionConfirmFrame(frame);
                        if (rc == kS7Ok) {
                            setupFrame = wrapS7Payload(runtime->client->buildSetupCommunicationRequest());
                        }
                    }
                }
                if (rc != kS7Ok || setupFrame.empty()) {
                    completeAsyncConnect(runtime, connect, false);
                    continue;
                }
                connect->stage = S7DeviceRuntime::AsyncConnect::Stage::SetupCommunication;
                if (!sendAsyncSessionFrame(runtime, setupFrame, false)) {
                    completeAsyncConnect(runtime, connect, false);
                }
                continue;
            }

            if (frame.size() < 7 || frame[4] != kCotpDtLength || frame[5] != kCotpDt) {
                completeAsyncConnect(runtime, connect, false);
                continue;
            }

            std::vector<uint8_t> payload(frame.begin() + 7, frame.end());
            int rc = kS7ErrInvalidHandle;
            {
                std::lock_guard clientLock(runtime->clientMutex);
                if (runtime->client) {
                    rc = runtime->client->parseSetupCommunicationResponse(payload);
                }
            }
            completeAsyncConnect(runtime, connect, rc == kS7Ok);
        }
    }

    bool startAsyncConnectDevice(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        std::function<void(bool)> complete) {

        if (!runtime || !complete) {
            return false;
        }

        int deviceId = 0;
        bool tcpServerMode = false;
        bool alreadyConnected = false;
        S7ConnectionConfig connection;
        std::uint64_t attemptId = 0;

        {
            std::lock_guard lock(runtime->mutex);
            deviceId = runtime->deviceId;
            tcpServerMode = runtime->tcpServerMode;
            connection = runtime->connection;
            alreadyConnected = runtime->connected
                && (!runtime->tcpServerMode || isSessionReadyLocked(*runtime));
        }
        if (alreadyConnected) {
            std::lock_guard clientLock(runtime->clientMutex);
            alreadyConnected = runtime->client && runtime->client->connected();
        }
        if (alreadyConnected) {
            complete(true);
            return true;
        }

        {
            std::scoped_lock lock(runtime->clientMutex, runtime->mutex);
            if (runtime->connectInProgress || runtime->asyncConnect) {
                return false;
            }
            if (!tcpServerMode) {
                LOG_WARN << "[S7][Adapter] Direct S7 TCP async client is not implemented yet: "
                         << deviceLabel(*runtime) << "(id=" << runtime->deviceId << ")"
                         << ", host=" << connection.host;
                return false;
            }

            runtime->resetClient();
            runtime->connectInProgress = true;
            runtime->connected = false;
            runtime->lastConnectAttempt = std::chrono::steady_clock::now();
            attemptId = ++runtime->connectGeneration;
        }

        auto client = std::make_unique<Client>();
        applyConnectionTimeouts(*client, connection);
        {
            std::lock_guard runtimeLock(runtime->mutex);
            const auto sourceRef = kSourceRefCandidates[runtime->sourceRefIndex % kSourceRefCandidates.size()];
            runtime->sourceRefIndex = (runtime->sourceRefIndex + 1) % kSourceRefCandidates.size();
            client->setConnectionSourceRef(sourceRef);
        }
        const std::string deviceName = runtime->deviceName;
        client->setTraceCallback(
            [deviceId, deviceName](std::string_view stage, bool outbound, const std::vector<std::uint8_t>& frame) {
                const std::string summary = summarizePacket(stage, outbound, frame);
                LOG_DEBUG << "[S7][Packet] " << (deviceName.empty() ? "S7-unknown" : deviceName)
                          << "(id=" << deviceId << ")"
                          << " " << (outbound ? "TX " : "RX ")
                          << stage
                          << " len=" << frame.size()
                          << summary
                          << " hex=" << bytesToHex(frame);
            });
        client->setTransportHooks({
            .close = []() {},
            .connected = [runtime]() {
                std::lock_guard lock(runtime->mutex);
                return isSessionReadyLocked(*runtime);
            }
        });

        int rc = kS7Ok;
        if (connection.mode == "TSAP") {
            rc = client->setConnectionParams(
                connection.host.c_str(),
                connection.localTSAP,
                connection.remoteTSAP);
        } else {
            rc = client->setConnectionType(connectionTypeToCode(connection.connectionType));
            if (rc == kS7Ok) {
                const auto remoteTsap = static_cast<std::uint16_t>(
                    (connectionTypeToCode(connection.connectionType) << 8)
                    + (connection.rack * 0x20)
                    + connection.slot);
                rc = client->setConnectionParams(connection.host.c_str(), connection.localTSAP, remoteTsap);
            }
        }
        if (rc != kS7Ok) {
            {
                std::lock_guard lock(runtime->mutex);
                runtime->connectInProgress = false;
                runtime->connected = false;
                runtime->lastConnectAttempt = std::chrono::steady_clock::now();
            }
            return false;
        }

        std::vector<uint8_t> frame;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            runtime->client = std::move(client);
            runtime->client->resetAsyncSessionState();
            frame = runtime->client->buildConnectionRequestFrame();
        }

        auto connect = std::make_shared<S7DeviceRuntime::AsyncConnect>();
        connect->attemptId = attemptId;
        connect->complete = std::move(complete);
        {
            std::lock_guard lock(runtime->mutex);
            if (runtime->connectGeneration != attemptId) {
                runtime->connectInProgress = false;
                runtime->connected = false;
                runtime->lastConnectAttempt = std::chrono::steady_clock::now();
                return false;
            }
            runtime->asyncConnect = connect;
        }

        auto loop = TcpLinkManager::instance().getNextIoLoop();
        if (loop) {
            const double timeoutSec = static_cast<double>(std::max(1000, connection.recvTimeoutMs)) / 1000.0;
            connect->timerId = loop->runAfter(timeoutSec, [this, runtime, connect]() {
                completeAsyncConnect(runtime, connect, false);
            });
        }

        if (!sendAsyncSessionFrame(runtime, frame, true)) {
            completeAsyncConnect(runtime, connect, false);
        }
        return true;
    }

    void processAsyncSessionBytes(const std::shared_ptr<S7DeviceRuntime>& runtime, std::vector<uint8_t> bytes) {
        if (!runtime || bytes.empty()) {
            return;
        }

        std::vector<std::pair<std::shared_ptr<S7DeviceRuntime::AsyncExchange>, std::vector<std::uint8_t>>> completed;
        int protocolError = kS7Ok;
        std::shared_ptr<S7DeviceRuntime::AsyncExchange> failedExchange;

        {
            std::lock_guard lock(runtime->mutex);
            runtime->asyncRxBuffer.insert(runtime->asyncRxBuffer.end(), bytes.begin(), bytes.end());

            while (runtime->asyncRxBuffer.size() >= 4) {
                if (runtime->asyncRxBuffer[0] != kIsoTcpVersion) {
                    protocolError = kS7ErrProtocol;
                    failedExchange = runtime->asyncExchange;
                    runtime->asyncRxBuffer.clear();
                    break;
                }

                const std::uint16_t totalLength = static_cast<std::uint16_t>(
                    (static_cast<std::uint16_t>(runtime->asyncRxBuffer[2]) << 8)
                    | runtime->asyncRxBuffer[3]);
                if (totalLength < 7) {
                    protocolError = kS7ErrProtocol;
                    failedExchange = runtime->asyncExchange;
                    runtime->asyncRxBuffer.clear();
                    break;
                }
                if (runtime->asyncRxBuffer.size() < totalLength) {
                    break;
                }

                std::vector<std::uint8_t> frame;
                frame.reserve(totalLength);
                for (std::uint16_t i = 0; i < totalLength; ++i) {
                    frame.push_back(runtime->asyncRxBuffer.front());
                    runtime->asyncRxBuffer.pop_front();
                }

                if (frame[4] != kCotpDtLength || frame[5] != kCotpDt || frame.size() < 19) {
                    protocolError = kS7ErrProtocol;
                    failedExchange = runtime->asyncExchange;
                    break;
                }

                std::vector<std::uint8_t> payload(frame.begin() + 7, frame.end());
                auto exchange = runtime->asyncExchange;
                if (!exchange) {
                    LOG_DEBUG << "[S7][Adapter] Drop async response without pending exchange: "
                              << deviceLabel(*runtime)
                              << "(id=" << runtime->deviceId << ")"
                              << ", bytes=" << frame.size();
                    continue;
                }

                if (payload.size() < 12 || readBe16(payload.data() + 4) != exchange->pduRef) {
                    LOG_DEBUG << "[S7][Adapter] Ignore async response with unmatched pduRef: "
                              << deviceLabel(*runtime)
                              << "(id=" << runtime->deviceId << ")"
                              << ", expected=" << exchange->pduRef
                              << ", got=" << (payload.size() >= 6 ? readBe16(payload.data() + 4) : 0);
                    continue;
                }

                const std::uint16_t parLen = readBe16(payload.data() + 6);
                const std::size_t functionOffset = payload[1] == kS7AckData ? 12 : 10;
                if (payload.size() < functionOffset + 1
                    || parLen == 0
                    || functionOffset + parLen > payload.size()
                    || payload[functionOffset] != exchange->expectedFunction) {
                    protocolError = kS7ErrFunctionMismatch;
                    failedExchange = exchange;
                    break;
                }

                completed.emplace_back(exchange, std::move(payload));
            }
        }

        if (protocolError != kS7Ok && failedExchange) {
            completeAsyncExchange(runtime, failedExchange, protocolError, {});
        }

        for (auto& [exchange, payload] : completed) {
            completeAsyncExchange(runtime, exchange, kS7Ok, std::move(payload));
        }
    }

    bool startAsyncSessionExchange(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        std::string responseStage,
        std::vector<std::uint8_t> payload,
        std::uint8_t expectedFunction,
        int timeoutMs,
        std::function<void(int, std::vector<std::uint8_t>)> complete) {

        if (!runtime || payload.size() < 6 || !complete) {
            return false;
        }

        int linkId = 0;
        std::string clientAddr;
        std::string deviceName;
        int deviceId = 0;
        const std::uint16_t pduRef = readBe16(payload.data() + 4);
        auto exchange = std::make_shared<S7DeviceRuntime::AsyncExchange>();
        exchange->pduRef = pduRef;
        exchange->expectedFunction = expectedFunction;
        exchange->responseStage = std::move(responseStage);
        exchange->complete = std::move(complete);

        {
            std::lock_guard lock(runtime->mutex);
            if (!isSessionReadyLocked(*runtime) || runtime->asyncExchange) {
                return false;
            }
            runtime->asyncExchange = exchange;
            linkId = runtime->sessionLinkId;
            clientAddr = runtime->sessionClientAddr;
            deviceName = runtime->deviceName;
            deviceId = runtime->deviceId;
        }

        auto loop = TcpLinkManager::instance().getNextIoLoop();
        if (loop) {
            const double timeoutSec = static_cast<double>(std::max(1, timeoutMs)) / 1000.0;
            exchange->timerId = loop->runAfter(timeoutSec, [this, runtime, exchange]() {
                {
                    std::lock_guard lock(runtime->mutex);
                    if (exchange->done || runtime->asyncExchange != exchange) {
                        return;
                    }
                }
                LOG_WARN << "[S7][Adapter] Async exchange timeout: "
                         << deviceLabel(*runtime) << "(id=" << runtime->deviceId << ")"
                         << ", stage=" << exchange->responseStage
                         << ", pduRef=" << exchange->pduRef;
                completeAsyncExchange(runtime, exchange, kS7ErrTimeout, {});
            });
        }

        auto frame = wrapS7Payload(payload);
        LOG_DEBUG << "[S7][Packet] " << (deviceName.empty() ? "S7-unknown" : deviceName)
                  << "(id=" << deviceId << ") TX async len=" << frame.size()
                  << summarizePacket("s7.async.req", true, frame)
                  << " hex=" << bytesToHex(frame);

        if (LinkTransportFacade::instance().sendToClient(linkId, clientAddr, bytesToString(frame))) {
            return true;
        }

        LinkTransportFacade::instance().forceDisconnectServerClient(linkId, clientAddr);
        completeAsyncExchange(runtime, exchange, kS7ErrSocketIo, {});
        return true;
    }

    void forwardDtuPayload(int deviceId, std::vector<uint8_t> bytes) {
        if (bytes.empty()) {
            return;
        }

        const std::size_t byteCount = bytes.size();
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime || !runtime->tcpServerMode) {
            return;
        }

        int linkId = 0;
        std::string sessionClientAddr;
        bool accepted = false;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            linkId = runtime->linkId;
            sessionClientAddr = runtime->sessionClientAddr;
            if (runtime->asyncConnect) {
                accepted = true;
            } else if (runtime->asyncExchange) {
                accepted = true;
            }
        }
        if (accepted) {
            bool isConnectPayload = false;
            {
                std::lock_guard runtimeLock(runtime->mutex);
                isConnectPayload = static_cast<bool>(runtime->asyncConnect);
            }
            if (isConnectPayload) {
                processAsyncConnectBytes(runtime, std::move(bytes));
            } else {
                processAsyncSessionBytes(runtime, std::move(bytes));
            }
            return;
        }

        if (!accepted) {
            LOG_DEBUG << "[S7][Adapter] Drop session payload because session transport is not open: "
                      << deviceLabel(*runtime) << "(id=" << deviceId << ")"
                      << ", linkId=" << linkId
                      << ", bytes=" << byteCount;
        }
    }

    bool enqueueScheduledPoll(int deviceId) {
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime) {
            return false;
        }

        bool hasAreas = false;
        bool tcpServerMode = false;
        bool sessionBound = false;
        int linkId = 0;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            hasAreas = !runtime->areas.empty();
            tcpServerMode = runtime->tcpServerMode;
            sessionBound = runtime->sessionBound;
            linkId = runtime->linkId;
        }
        if (!hasAreas) {
            return false;
        }
        if (tcpServerMode && !sessionBound) {
            bool hasSessionCandidate = false;
            if (sessionManager_) {
                auto sessions = sessionManager_->listSessions();
                hasSessionCandidate = std::any_of(
                    sessions.begin(),
                    sessions.end(),
                    [linkId](const S7DtuSession& session) {
                        return session.linkId == linkId
                            && !session.clientAddr.empty()
                            && session.bindState != SessionBindState::Bound;
                    });
            }
            if (!hasSessionCandidate) {
                return false;
            }
        }

        drogon::async_run([this, deviceId]() -> Task<> {
            co_await runScheduledPoll(deviceId);
        });
        return true;
    }

    void completeAsyncWriteCommand(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<AsyncCommandWriteContext>& context,
        std::function<void(std::string)> complete,
        std::string failure) {
        (void)runtime;
        (void)context;
        if (complete) {
            complete(std::move(failure));
        }
    }

    void startAsyncWriteNext(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<AsyncCommandWriteContext>& context,
        int recvTimeoutMs,
        std::function<void(std::string)> complete) {

        while (context->nextWrite < context->writes.size()
               && context->writes[context->nextWrite].writeBytes.empty()) {
            ++context->nextWrite;
        }
        if (context->nextWrite >= context->writes.size()) {
            completeAsyncWriteCommand(runtime, context, std::move(complete), {});
            return;
        }

        auto& write = context->writes[context->nextWrite++];
        DataItem item{
            .area = areaToCode(write.area->area),
            .dbNumber = resolvedDbNumber(*write.area),
            .start = write.area->start,
            .amount = transferAmount(write.area->area, write.writeBytes.size()),
            .wordLen = areaWordLen(write.area->area),
            .data = write.writeBytes.data(),
            .capacity = write.writeBytes.size()
        };

        std::vector<DataItem> items{item};
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            if (!runtime->client) {
                completeAsyncWriteCommand(runtime, context, std::move(complete), "S7 设备离线");
                return;
            }
            if (!writeItemFitsPdu(items[0], runtime->client->negotiatedPduLength())) {
                completeAsyncWriteCommand(runtime, context, std::move(complete),
                    "S7 写入数据超过协商 PDU: " + write.area->name);
                return;
            }
            payload = runtime->client->buildWriteRequestForItems(items, 0, 1);
        }

        if (!startAsyncSessionExchange(
                runtime,
                "s7.write.resp",
                std::move(payload),
                kS7FuncWrite,
                recvTimeoutMs,
                [this, runtime, context, items = std::move(items), areaName = write.area->name, recvTimeoutMs, complete = std::move(complete)](
                    int rc,
                    std::vector<std::uint8_t> response) mutable {
                    int parseRc = rc;
                    auto parsedItems = items;
                    if (rc == kS7Ok) {
                        std::lock_guard clientLock(runtime->clientMutex);
                        parseRc = runtime->client
                            ? runtime->client->parseWriteResponseForItems(response, parsedItems, 0, 1)
                            : kS7ErrInvalidHandle;
                    }
                    if (parseRc != kS7Ok || parsedItems[0].result != kS7Ok) {
                        const int finalRc = parsedItems[0].result != kS7Ok ? parsedItems[0].result : parseRc;
                        completeAsyncWriteCommand(runtime, context, std::move(complete),
                            "S7 写入失败: " + areaName + "，错误码="
                            + std::to_string(finalRc) + " (" + explainClientRc(finalRc) + ")");
                        return;
                    }
                    startAsyncWriteNext(runtime, context, recvTimeoutMs, std::move(complete));
                })) {
            completeAsyncWriteCommand(runtime, context, std::move(complete), "S7 写入发送失败");
        }
    }

    void startAsyncWriteReadBatch(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<AsyncCommandWriteContext>& context,
        int recvTimeoutMs,
        std::function<void(std::string)> complete) {

        if (context->nextReadBatch >= context->readBatches.size()) {
            for (auto& write : context->writes) {
                auto bytes = encodeAreaValue(
                    *write.area,
                    write.element,
                    write.existingBytes.empty() ? nullptr : &write.existingBytes);
                if (!bytes || bytes->empty()) {
                    completeAsyncWriteCommand(runtime, context, std::move(complete), "S7 指令值格式错误");
                    return;
                }
                write.writeBytes = std::move(*bytes);
            }
            startAsyncWriteNext(runtime, context, recvTimeoutMs, std::move(complete));
            return;
        }

        const auto [begin, end] = context->readBatches[context->nextReadBatch++];
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            if (!runtime->client) {
                completeAsyncWriteCommand(runtime, context, std::move(complete), "S7 设备离线");
                return;
            }
            payload = runtime->client->buildReadRequestForItems(context->readItems, begin, end);
        }

        if (!startAsyncSessionExchange(
                runtime,
                "s7.read.multi.resp",
                std::move(payload),
                kS7FuncRead,
                recvTimeoutMs,
                [this, runtime, context, begin, end, recvTimeoutMs, complete = std::move(complete)](
                    int rc,
                    std::vector<std::uint8_t> response) mutable {
                    int parseRc = rc;
                    if (rc == kS7Ok) {
                        std::lock_guard clientLock(runtime->clientMutex);
                        parseRc = runtime->client
                            ? runtime->client->parseReadResponseForItems(response, context->readItems, begin, end)
                            : kS7ErrInvalidHandle;
                    }
                    if (parseRc != kS7Ok) {
                        completeAsyncWriteCommand(runtime, context, std::move(complete),
                            "S7 读取当前位值失败，错误码="
                            + std::to_string(parseRc) + " (" + explainClientRc(parseRc) + ")");
                        return;
                    }
                    startAsyncWriteReadBatch(runtime, context, recvTimeoutMs, std::move(complete));
                })) {
            completeAsyncWriteCommand(runtime, context, std::move(complete), "S7 读取当前位值发送失败");
        }
    }

    void startAsyncWriteCommand(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        Json::Value elements,
        std::function<void(std::string)> complete) {
        if (!runtime) {
            complete("S7 设备离线");
            return;
        }

        int recvTimeoutMs = kDefaultS7RecvTimeoutMs;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            recvTimeoutMs = runtime->connection.recvTimeoutMs;
            if (!isSessionReadyLocked(*runtime)) {
                complete("S7 会话未就绪");
                return;
            }
        }

        auto context = std::make_shared<AsyncCommandWriteContext>();
        context->writes.reserve(static_cast<std::size_t>(elements.size()));

        for (const auto& elem : elements) {
            if (!elem.isObject()) {
                continue;
            }
            const auto* areaDef = findAreaDefinition(*runtime, elem);
            if (!areaDef) {
                complete("未找到 S7 要素对应的写入地址");
                return;
            }
            if (!areaDef->writable) {
                complete("S7 要素未启用写入: " + areaDef->name);
                return;
            }

            if (normalizeDataType(areaDef->dataType) == "BOOL"
                && elem.get("valueHex", "").asString().empty()) {
                bool boolValue = false;
                if (!parseBoolValue(jsonValueToString(elem["value"]), boolValue)) {
                    complete("S7 指令值格式错误");
                    return;
                }
            }

            context->writes.push_back(S7PreparedWrite{.area = areaDef, .element = elem});
        }

        std::uint16_t pduLength = 0;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            pduLength = runtime->client ? runtime->client->negotiatedPduLength() : 0;
        }
        if (pduLength == 0) {
            complete("S7 设备离线");
            return;
        }

        for (auto& write : context->writes) {
            if (normalizeDataType(write.area->dataType) != "BOOL"
                || !write.element.get("valueHex", "").asString().empty()) {
                continue;
            }
            write.existingBytes.assign(static_cast<std::size_t>(std::max(1, write.area->size)), 0);
            context->readItems.push_back(DataItem{
                .area = areaToCode(write.area->area),
                .dbNumber = resolvedDbNumber(*write.area),
                .start = write.area->start,
                .amount = transferAmount(write.area->area, write.existingBytes.size()),
                .wordLen = areaWordLen(write.area->area),
                .data = write.existingBytes.data(),
                .capacity = write.existingBytes.size()
            });
        }

        if (!context->readItems.empty()) {
            context->readBatches = buildAsyncReadBatches(context->readItems, pduLength);
            if (context->readBatches.empty()) {
                complete("S7 读取当前位值超过协商 PDU");
                return;
            }
        }
        startAsyncWriteReadBatch(runtime, context, recvTimeoutMs, std::move(complete));
    }

    static std::vector<std::pair<std::size_t, std::size_t>> buildAsyncReadBatches(
        const std::vector<DataItem>& items,
        std::uint16_t pduLength) {
        std::vector<std::pair<std::size_t, std::size_t>> batches;
        std::size_t begin = 0;
        while (begin < items.size()) {
            if (!readItemFitsPdu(items[begin], pduLength)) {
                return {};
            }

            std::size_t end = begin + 1;
            while (end < items.size()) {
                const std::size_t nextCount = end - begin + 1;
                if (readRequestPduSize(nextCount) > pduLength
                    || readResponsePduSize(items, begin, end + 1) > pduLength) {
                    break;
                }
                ++end;
            }
            batches.emplace_back(begin, end);
            begin = end;
        }
        return batches;
    }

    void finalizeAsyncPoll(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<AsyncPollContext>& context,
        std::function<void()> done) {

        std::vector<ParsedFrameResult> parsedResults;
        Json::Value aggregatedData(Json::objectValue);
        bool anyBlockSucceeded = false;
        bool pollFailed = context->pollFailed;

        for (std::size_t blockIndex = 0; blockIndex < context->plans.size(); ++blockIndex) {
            const auto& block = context->plans[blockIndex];
            auto& buffer = context->buffers[blockIndex];
            const int blockRc = context->results[blockIndex];
            if (blockRc != kS7Ok) {
                pollFailed = true;
                LOG_WARN << "[S7][Adapter] Async RX block failed: "
                         << deviceLabel(*runtime) << "(id=" << context->deviceId << ")"
                         << ", area=" << block.area
                         << ", db=" << block.dbNumber
                         << ", start=" << block.start
                         << ", amount=" << block.amount
                         << ", rc=" << blockRc
                         << " (" << explainClientRc(blockRc) << ")";
                continue;
            }

            LOG_DEBUG << "[S7][Adapter] RX block OK: " << deviceLabel(*runtime)
                      << "(id=" << context->deviceId << ")"
                      << ", area=" << block.area
                      << ", db=" << block.dbNumber
                      << ", start=" << block.start
                      << ", amount=" << block.amount
                      << ", bytes=" << buffer.size()
                      << ", mergedAreas=" << block.members.size()
                      << ", async=yes";

            for (const auto& member : block.members) {
                if (!member.area || member.offsetBytes + static_cast<std::size_t>(member.area->size) > buffer.size()) {
                    pollFailed = true;
                    continue;
                }

                std::vector<uint8_t> areaBuffer(
                    buffer.begin() + static_cast<std::ptrdiff_t>(member.offsetBytes),
                    buffer.begin() + static_cast<std::ptrdiff_t>(member.offsetBytes + member.area->size));
                aggregatedData[member.area->id] = buildReadElement(*member.area, areaBuffer);
                anyBlockSucceeded = true;
            }
        }

        if (pollFailed && !anyBlockSucceeded) {
            std::lock_guard clientLock(runtime->clientMutex);
            runtime->resetClient();
        }

        if (aggregatedData.isObject() && !aggregatedData.empty()) {
            parsedResults.push_back(buildPollReadResult(
                context->deviceId,
                context->linkId,
                std::move(aggregatedData),
                context->reportTime));
        }

        if (!parsedResults.empty() && runtimeContext_.submitParsedResults) {
            runtimeContext_.submitParsedResults(std::move(parsedResults));
        }
        if (pollScheduler_) {
            pollScheduler_->onPollCompleted(context->deviceId, anyBlockSucceeded);
        }
        if (done) {
            done();
        }
    }

    void startAsyncPollBatch(
        const std::shared_ptr<S7DeviceRuntime>& runtime,
        const std::shared_ptr<AsyncPollContext>& context,
        int recvTimeoutMs,
        std::function<void()> done) {

        if (context->nextBatch >= context->batches.size()) {
            finalizeAsyncPoll(runtime, context, std::move(done));
            return;
        }

        const auto [begin, end] = context->batches[context->nextBatch++];
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            if (!runtime->client) {
                context->pollFailed = true;
                for (std::size_t i = begin; i < end; ++i) {
                    context->results[context->indexes[i]] = kS7ErrInvalidHandle;
                }
                finalizeAsyncPoll(runtime, context, std::move(done));
                return;
            }
            payload = runtime->client->buildReadRequestForItems(context->items, begin, end);
        }

        LOG_DEBUG << "[S7][Adapter] MultiVar read batch: " << deviceLabel(*runtime)
                  << "(id=" << context->deviceId << "), items=" << (end - begin)
                  << ", batch=" << context->nextBatch << "/" << context->batches.size()
                  << ", async=yes";

        if (!startAsyncSessionExchange(
                runtime,
                "s7.read.multi.resp",
                std::move(payload),
                kS7FuncRead,
                recvTimeoutMs,
                [this, runtime, context, begin, end, recvTimeoutMs, done = std::move(done)](
                    int rc,
                    std::vector<std::uint8_t> response) mutable {
                    int parseRc = rc;
                    if (rc == kS7Ok) {
                        std::lock_guard clientLock(runtime->clientMutex);
                        parseRc = runtime->client
                            ? runtime->client->parseReadResponseForItems(response, context->items, begin, end)
                            : kS7ErrInvalidHandle;
                    }
                    if (parseRc != kS7Ok) {
                        context->pollFailed = true;
                    }
                    for (std::size_t i = begin; i < end; ++i) {
                        context->results[context->indexes[i]] = context->items[i].result == kS7Ok
                            ? (parseRc < kS7Ok ? parseRc : kS7Ok)
                            : context->items[i].result;
                    }
                    startAsyncPollBatch(runtime, context, recvTimeoutMs, std::move(done));
                })) {
            context->pollFailed = true;
            for (std::size_t i = begin; i < end; ++i) {
                context->results[context->indexes[i]] = kS7ErrSocketIo;
            }
            finalizeAsyncPoll(runtime, context, std::move(done));
        }
    }

    bool startAsyncPollDevice(const std::shared_ptr<S7DeviceRuntime>& runtime, std::function<void()> done) {
        if (!runtime) {
            return false;
        }

        int deviceId = 0;
        int linkId = 0;
        bool tcpServerMode = false;
        bool sessionReady = false;
        int recvTimeoutMs = kDefaultS7RecvTimeoutMs;
        std::vector<S7AreaDefinition> areas;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            deviceId = runtime->deviceId;
            linkId = runtime->linkId;
            tcpServerMode = runtime->tcpServerMode;
            sessionReady = isSessionReadyLocked(*runtime);
            recvTimeoutMs = runtime->connection.recvTimeoutMs;
            areas = runtime->areas;
        }
        if (!tcpServerMode || !sessionReady || areas.empty()) {
            return false;
        }

        const auto readPlans = planReadBlocks(areas);
        if (readPlans.empty()) {
            return false;
        }

        auto context = std::make_shared<AsyncPollContext>();
        context->deviceId = deviceId;
        context->linkId = linkId;
        context->reportTime = makeUtcNowString();
        context->plans = readPlans;
        context->buffers.reserve(readPlans.size());
        context->items.reserve(readPlans.size());
        context->results.assign(readPlans.size(), kS7ErrTimeout);

        for (std::size_t index = 0; index < readPlans.size(); ++index) {
            const auto& block = readPlans[index];
            auto& buffer = context->buffers.emplace_back(block.byteSize, 0);
            context->items.push_back(DataItem{
                .area = block.areaCode,
                .dbNumber = block.dbNumber,
                .start = block.start,
                .amount = block.amount,
                .wordLen = block.wordLen,
                .data = buffer.data(),
                .capacity = buffer.size()
            });
            context->indexes.push_back(index);
        }

        std::uint16_t pduLength = 0;
        {
            std::lock_guard clientLock(runtime->clientMutex);
            pduLength = runtime->client ? runtime->client->negotiatedPduLength() : 0;
        }
        if (pduLength == 0) {
            LOG_WARN << "[S7][Adapter] Async poll batch does not fit PDU, fallback required: "
                     << deviceLabel(*runtime) << "(id=" << deviceId << ")"
                     << ", items=" << context->items.size()
                     << ", pduLength=" << pduLength;
            return false;
        }
        context->batches = buildAsyncReadBatches(context->items, pduLength);
        if (context->batches.empty()) {
            LOG_WARN << "[S7][Adapter] Async poll item exceeds negotiated PDU: "
                     << deviceLabel(*runtime) << "(id=" << deviceId << ")"
                     << ", items=" << context->items.size()
                     << ", pduLength=" << pduLength;
            return false;
        }

        LOG_DEBUG << "[S7][Adapter] Poll " << deviceLabel(*runtime)
                  << "(id=" << deviceId << ")"
                  << ", areas=" << areas.size()
                  << ", blocks=" << readPlans.size()
                  << ", session=yes, bound=yes, async=yes";
        startAsyncPollBatch(runtime, context, recvTimeoutMs, std::move(done));
        return true;
    }

    Task<> runScheduledPoll(int deviceId) {
        auto runtime = findRuntimeLocked(deviceId);
        if (!runtime) {
            if (pollScheduler_) {
                pollScheduler_->onPollCompleted(deviceId, false);
            }
            co_return;
        }

        bool tcpServerMode = false;
        bool sessionReady = false;
        bool sessionBound = false;
        int linkId = 0;
        {
            std::lock_guard runtimeLock(runtime->mutex);
            tcpServerMode = runtime->tcpServerMode;
            sessionReady = isSessionReadyLocked(*runtime);
            sessionBound = runtime->sessionBound
                && runtime->sessionLinkId > 0
                && !runtime->sessionClientAddr.empty();
            linkId = runtime->linkId;
        }

        auto enqueuePollOnDeviceQueue = [this, runtime, deviceId]() {
            if (!enqueueDeviceOperation(
                    runtime,
                    ProtocolJobPriority::Normal,
                    [this, runtime, deviceId](std::function<void()> done) mutable {
                        auto donePtr = std::make_shared<std::function<void()>>(std::move(done));
                        auto finishFailure = [this, deviceId, donePtr]() mutable {
                            if (pollScheduler_) {
                                pollScheduler_->onPollCompleted(deviceId, false);
                            }
                            if (donePtr && *donePtr) {
                                auto fn = std::move(*donePtr);
                                *donePtr = nullptr;
                                fn();
                            }
                        };
                        if (!startAsyncConnectDevice(
                                runtime,
                                [this, runtime, deviceId, donePtr](bool connected) mutable {
                                    if (!connected) {
                                        if (pollScheduler_) {
                                            pollScheduler_->onPollCompleted(deviceId, false);
                                        }
                                        if (donePtr && *donePtr) {
                                            auto fn = std::move(*donePtr);
                                            *donePtr = nullptr;
                                            fn();
                                        }
                                        return;
                                    }
                                    if (donePtr && *donePtr) {
                                        auto pollDone = [donePtr]() mutable {
                                            if (donePtr && *donePtr) {
                                                auto fn = std::move(*donePtr);
                                                *donePtr = nullptr;
                                                fn();
                                            }
                                        };
                                        if (!startAsyncPollDevice(runtime, std::move(pollDone))) {
                                            if (pollScheduler_) {
                                                pollScheduler_->onPollCompleted(deviceId, false);
                                            }
                                            if (donePtr && *donePtr) {
                                                auto fn = std::move(*donePtr);
                                                *donePtr = nullptr;
                                                fn();
                                            }
                                        }
                                    }
                                })) {
                            finishFailure();
                        }
                    })) {
                if (pollScheduler_) {
                    pollScheduler_->onPollCompleted(deviceId, false);
                }
            }
        };

        if (!tcpServerMode) {
            LOG_WARN << "[S7][Adapter] Direct S7 TCP poll skipped because non-blocking TcpClient path is not enabled: "
                     << deviceLabel(*runtime) << "(id=" << deviceId << ")";
            if (pollScheduler_) {
                pollScheduler_->onPollCompleted(deviceId, false);
            }
            co_return;
        }

        if (sessionReady || sessionBound) {
            enqueuePollOnDeviceQueue();
            co_return;
        }

        if (!enqueueLinkOperation(
                linkId,
                [this, runtime, deviceId, enqueuePollOnDeviceQueue](std::function<void()> done) mutable {
                    if (!startAsyncConnectDevice(
                            runtime,
                            [this, deviceId, enqueuePollOnDeviceQueue, done = std::move(done)](bool connected) mutable {
                                if (connected) {
                                    enqueuePollOnDeviceQueue();
                                } else if (pollScheduler_) {
                                    pollScheduler_->onPollCompleted(deviceId, false);
                                }
                                if (done) {
                                    done();
                                }
                            })) {
                        if (pollScheduler_) {
                            pollScheduler_->onPollCompleted(deviceId, false);
                        }
                        if (done) {
                            done();
                        }
                    }
                })) {
            if (pollScheduler_) {
                pollScheduler_->onPollCompleted(deviceId, false);
            }
        }
        co_return;
    }

    mutable std::mutex devicesMutex_;
    std::unordered_map<int, std::shared_ptr<S7DeviceRuntime>> devices_;
    mutable std::mutex linkOperationMutex_;
    std::unordered_map<int, LinkOperationRuntime> linkOperationQueues_;
    std::unique_ptr<DtuRegistry> sessionRegistry_;
    std::unique_ptr<DtuSessionManager> sessionManager_;
    std::unique_ptr<RegistrationNormalizer> sessionNormalizer_;
    std::unique_ptr<S7PollScheduler> pollScheduler_;
};

}  // namespace s7


