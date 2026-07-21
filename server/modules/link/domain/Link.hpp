#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/link/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/JsonHelper.hpp"
#include "common/utils/LinkHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"

#include <unordered_set>

/**
 * @brief 链路聚合根
 *
 * 管理 TCP Server/Client 链路配置。
 * TCP 连接的启停由 LinkEventHandlers 通过事件驱动自动处理。
 * 连接状态查询由 LinkService 通过 TcpLinkManager 注入。
 *
 * 使用示例：
 * @code
 * // 创建链路（事件处理器自动启动 TCP 连接）
 * co_await Link::create(data)
 *     .require(Link::nameUnique)
 *     .require(Link::endpointUnique)
 *     .save();
 *
 * // 更新链路（事件处理器自动重载连接）
 * co_await Link::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除链路（校验无关联设备，事件处理器自动停止 TCP 连接）
 * co_await Link::of(id)
 *     .require(Link::noDevices)
 *     .remove()
 *     .save();
 * @endcode
 */
class Link : public Aggregate<Link> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的链路
     */
    static Task<Link> of(int id) {
        Link link;
        co_await link.load(id);
        co_return link;
    }

    /**
     * @brief 创建新链路
     */
    static Link create(const Json::Value& data) {
        Link link;
        link.applyCreate(data);
        return link;
    }

    /**
     * @brief 分页查询链路列表（包含连接状态）
     */
    static Task<PagedResult<Link>> list(const Pagination& page, const std::string& mode = "") {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted("l.deleted_at");
        if (!page.keyword.empty()) qb.likeAny({"l.name", "l.ip", "l.targets::text"}, page.keyword);
        if (!mode.empty()) qb.eq("l.mode", mode);

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM link l" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据
        std::string sql = R"(
            SELECT l.*,
                   a.name as agent_name,
                   a.code as agent_code,
                   a.is_online as agent_online
            FROM link l
            LEFT JOIN agent_node a ON l.agent_id = a.id AND a.deleted_at IS NULL
        )" + qb.whereClause() + " ORDER BY l.id ASC" + page.limitClause();
        auto result = co_await db.execSqlCoro(sql, qb.params());

        std::vector<Link> links;
        links.reserve(result.size());
        for (const auto& row : result) {
            Link link;
            link.fromRow(row);
            links.push_back(std::move(link));
        }

        co_return PagedResult<Link>{std::move(links), total, page.page, page.pageSize};
    }

    /**
     * @brief 获取所有链路选项（下拉选择用）
     */
    static Task<Json::Value> options() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            R"(
                SELECT l.id, l.name, l.mode, l.protocol, l.ip, l.port, l.targets, l.usage,
                       l.agent_id, l.agent_interface, l.agent_bind_ip,
                       l.agent_prefix_length, l.agent_gateway,
                       a.name as agent_name, a.code as agent_code, a.is_online as agent_online
                FROM link l
                LEFT JOIN agent_node a ON l.agent_id = a.id AND a.deleted_at IS NULL
                WHERE l.deleted_at IS NULL
                ORDER BY l.name ASC
            )"
        );

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            const auto name = FieldHelper::getString(row["name"]);
            const auto usage = FieldHelper::getString(row["usage"], Constants::LINK_USAGE_DEVICE);
            if (LinkHelper::isReservedAgentLink(name, usage)) {
                continue;
            }

            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = name;
            item["mode"] = FieldHelper::getString(row["mode"]);
            item["protocol"] = FieldHelper::getString(row["protocol"], Constants::PROTOCOL_SL651);
            item["ip"] = FieldHelper::getString(row["ip"]);
            item["port"] = FieldHelper::getInt(row["port"]);
            item["targets"] = parseJsonArrayField(row["targets"]);
            item["usage"] = usage;
            item["agent_id"] = FieldHelper::getInt(row["agent_id"]);
            item["agent_interface"] = FieldHelper::getString(row["agent_interface"]);
            item["agent_bind_ip"] = FieldHelper::getString(row["agent_bind_ip"]);
            item["agent_prefix_length"] = row["agent_prefix_length"].isNull()
                ? Json::nullValue
                : Json::Value(FieldHelper::getInt(row["agent_prefix_length"]));
            item["agent_gateway"] = FieldHelper::getString(row["agent_gateway"]);
            item["agent_name"] = FieldHelper::getString(row["agent_name"]);
            item["agent_code"] = FieldHelper::getString(row["agent_code"]);
            item["agent_online"] = FieldHelper::getBool(row["agent_online"]);
            items.append(item);
        }
        co_return items;
    }

    /**
     * @brief 查询所有已启用的链路（服务器启动时用于批量启动）
     */
    static Task<std::vector<Link>> findAllEnabled() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            R"(
                SELECT l.*,
                       a.name as agent_name,
                       a.code as agent_code,
                       a.is_online as agent_online
                FROM link l
                LEFT JOIN agent_node a ON l.agent_id = a.id AND a.deleted_at IS NULL
                WHERE l.status = 'enabled' AND l.deleted_at IS NULL
            )"
        );

        std::vector<Link> links;
        links.reserve(result.size());
        for (const auto& row : result) {
            Link link;
            link.fromRow(row);
            links.push_back(std::move(link));
        }
        co_return links;
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：链路名称唯一
     */
    static Task<void> nameUnique(const Link& link) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM link WHERE name = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {link.name_};

        if (link.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(link.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ConflictException("链路名称已存在");
        }
    }

    /**
     * @brief 约束：端点（mode + ip + port）唯一
     */
    static Task<void> endpointUnique(const Link& link) {
        // TCP Client 的远端地址存放在 targets JSONB 中，由 targetsValid 校验。
        if (link.mode_ == Constants::LINK_MODE_TCP_CLIENT) {
            co_return;
        }

        DatabaseService db;
        std::string sql = R"(
            SELECT 1
            FROM link
            WHERE mode = ? AND ip = ? AND port = ?
              AND COALESCE(agent_id, 0) = ?
              AND COALESCE(agent_bind_ip, '') = ?
              AND deleted_at IS NULL
        )";
        std::vector<std::string> params = {
            link.mode_,
            link.ip_,
            std::to_string(link.port_),
            std::to_string(link.agentId_),
            link.agentBindIp_
        };

        if (link.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(link.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ConflictException("相同模式、IP和端口的链路已存在");
        }
    }

    /** TCP Client 目标配置结构与同链路唯一性校验。 */
    static Task<void> targetsValid(const Link& link) {
        if (link.mode_ != Constants::LINK_MODE_TCP_CLIENT) {
            if (link.targets_.isArray() && !link.targets_.empty()) {
                throw ValidationException("TCP Server 模式不能配置目标地址");
            }
            co_return;
        }

        if (!link.targets_.isArray() || link.targets_.empty()) {
            throw ValidationException("TCP Client 模式至少需要配置一个目标地址");
        }

        std::unordered_set<std::string> ids;
        std::unordered_set<std::string> endpoints;
        for (const auto& target : link.targets_) {
            if (!target.isObject()) {
                throw ValidationException("目标地址格式错误");
            }
            const auto id = target.get("id", "").asString();
            const auto name = target.get("name", "").asString();
            const auto ip = target.get("ip", "").asString();
            const int port = target.get("port", 0).asInt();
            const auto status = target.get("status", Constants::USER_STATUS_ENABLED).asString();
            if (id.empty() || name.empty()) {
                throw ValidationException("目标名称和目标ID不能为空");
            }
            if (!isValidIpv4(ip)) {
                throw ValidationException("目标IP格式错误: " + ip);
            }
            if (port <= 0 || port > 65535) {
                throw ValidationException("目标端口范围必须为 1-65535");
            }
            if (status != Constants::USER_STATUS_ENABLED && status != Constants::USER_STATUS_DISABLED) {
                throw ValidationException("目标状态只能为 enabled 或 disabled");
            }
            if (!ids.insert(id).second) {
                throw ConflictException("目标ID重复: " + id);
            }
            const auto endpoint = ip + ":" + std::to_string(port);
            if (!endpoints.insert(endpoint).second) {
                throw ConflictException("同一链路下目标地址重复: " + endpoint);
            }
        }
        co_return;
    }

    /** 更新 targets 时禁止移除仍被设备引用的目标。 */
    static Task<void> assignedTargetsRetained(const Link& link) {
        if (link.id() <= 0 || link.mode_ != Constants::LINK_MODE_TCP_CLIENT) co_return;

        std::unordered_set<std::string> targetIds;
        for (const auto& target : link.targets_) {
            targetIds.insert(target.get("id", "").asString());
        }

        DatabaseService db;
        auto rows = co_await db.execSqlCoro(R"(
            SELECT DISTINCT protocol_params->>'target_id' AS target_id
            FROM device
            WHERE link_id = ? AND deleted_at IS NULL
              AND COALESCE(protocol_params->>'target_id', '') != ''
        )", {std::to_string(link.id())});
        for (const auto& row : rows) {
            const auto targetId = FieldHelper::getString(row["target_id"], "");
            if (!targetId.empty() && !targetIds.contains(targetId)) {
                throw ConflictException("目标仍有关联设备，无法删除: " + targetId);
            }
        }
        co_return;
    }

    static Task<void> agentExists(const Link& link) {
        if (link.agentId_ <= 0) {
            co_return;
        }

        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM agent_node WHERE id = ? AND deleted_at IS NULL",
            {std::to_string(link.agentId_)}
        );
        if (result.empty()) {
            throw NotFoundException("指定的采集Agent不存在");
        }
    }

    static Task<void> agentBindingValid(const Link& link) {
        if (link.agentId_ <= 0) {
            co_return;
        }
        if (link.agentBindIp_.empty()) {
            throw ValidationException("选择采集Agent执行时必须指定 Agent 网口IP");
        }
        if (link.agentInterface_.empty()) {
            throw ValidationException("选择采集Agent执行时必须指定网口名称");
        }
        if (!isValidIpv4(link.agentBindIp_)) {
            throw ValidationException("Agent 网口IP格式错误");
        }
        if (!isValidPrefixLength(link.agentPrefixLength_)) {
            throw ValidationException("Agent 网口前缀长度必须在 1-30 之间");
        }
        if (!link.agentGateway_.empty() && !isValidIpv4(link.agentGateway_)) {
            throw ValidationException("Agent 网关格式错误");
        }
        if (!link.agentGateway_.empty() && link.mode_ != Constants::LINK_MODE_TCP_CLIENT) {
            throw ValidationException("仅 TCP Client 模式支持配置 Agent 网关");
        }

        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            R"(
                SELECT capabilities
                FROM agent_node
                WHERE id = ? AND deleted_at IS NULL
                LIMIT 1
            )",
            {std::to_string(link.agentId_)}
        );
        if (result.empty()) {
            throw NotFoundException("指定的采集Agent不存在");
        }

        const auto capabilities = parseJsonField(result[0]["capabilities"]);
        const auto interfaces = capabilities.get("interfaces", Json::Value(Json::arrayValue));
        if (!interfaces.isArray() || interfaces.empty()) {
            throw ConflictException("采集Agent尚未上报可用网口，无法绑定链路");
        }

        const auto matchedInterface = findAgentInterfaceByName(interfaces, link.agentInterface_);
        if (!matchedInterface) {
            throw ConflictException("所选网口不属于采集Agent当前上报能力");
        }

        const auto reportedName = matchedInterface->get("name", "").asString();
        if (reportedName.empty() || reportedName != link.agentInterface_) {
            throw ConflictException("所选网口名称与采集Agent当前上报能力不一致");
        }

        std::string sql = R"(
            SELECT 1
            FROM link
            WHERE agent_id = ?
              AND agent_interface = ?
              AND (
                    COALESCE(agent_bind_ip, '') != ?
                    OR COALESCE(agent_prefix_length, 24) != ?
                  )
              AND deleted_at IS NULL
        )";
        std::vector<std::string> params = {
            std::to_string(link.agentId_),
            link.agentInterface_,
            link.agentBindIp_,
            std::to_string(link.agentPrefixLength_)
        };
        if (link.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(link.id()));
        }

        auto conflict = co_await db.execSqlCoro(sql, params);
        if (!conflict.empty()) {
            throw ConflictException("同一块 Agent 网口只能配置一套本机网络参数");
        }
        co_return;
    }

    /**
     * @brief 约束：Agent 链路为系统保留链路
     */
    static Task<void> notReserved(const Link& link) {
        if (link.isReserved()) {
            throw ForbiddenException("Agent链路为系统保留链路，不允许新增、编辑或删除");
        }
        co_return;
    }

    /**
     * @brief 约束：链路下无关联设备（删除前校验）
     */
    static Task<void> noDevices(const Link& link) {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT 1 FROM device WHERE link_id = ? AND deleted_at IS NULL LIMIT 1",
            {std::to_string(link.id())}
        );
        if (!result.empty()) {
            throw ConflictException("该链路下存在关联设备，无法删除");
        }
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新链路信息
     */
    Link& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        // mode 和 protocol 创建后不可修改（设备锁定了 link_id，换模式/协议会导致不一致）
        if (data.isMember("ip")) {
            ip_ = data["ip"].asString();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("port")) {
            port_ = data["port"].asInt();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("targets")) {
            targets_ = normalizeTargets(data["targets"]);
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("agent_id")) {
            agentId_ = data["agent_id"].isNull() ? 0 : data["agent_id"].asInt();
            if (agentId_ <= 0) {
                agentId_ = 0;
                agentInterface_.clear();
                agentBindIp_.clear();
            }
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("agent_interface")) {
            agentInterface_ = data["agent_interface"].asString();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("agent_bind_ip")) {
            agentBindIp_ = data["agent_bind_ip"].asString();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("agent_prefix_length")) {
            agentPrefixLength_ = data["agent_prefix_length"].isNull() ? 0 : data["agent_prefix_length"].asInt();
            markDirty();
            needReload_ = true;
        }
        if (data.isMember("agent_gateway")) {
            agentGateway_ = data["agent_gateway"].asString();
            markDirty();
            needReload_ = true;
        }
        normalizeAgentSettings();
        return *this;
    }

    /**
     * @brief 标记删除
     */
    Link& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& name() const { return name_; }
    const std::string& mode() const { return mode_; }
    const std::string& protocol() const { return protocol_; }
    const std::string& ip() const { return ip_; }
    int port() const { return port_; }
    const Json::Value& targets() const { return targets_; }
    const std::string& usage() const { return usage_; }
    const std::string& status() const { return status_; }
    int createdBy() const { return createdBy_; }
    int agentId() const { return agentId_; }
    const std::string& agentInterface() const { return agentInterface_; }
    const std::string& agentBindIp() const { return agentBindIp_; }
    int agentPrefixLength() const { return agentPrefixLength_; }
    const std::string& agentGateway() const { return agentGateway_; }
    const std::string& agentName() const { return agentName_; }
    const std::string& agentCode() const { return agentCode_; }
    bool agentOnline() const { return agentOnline_; }
    bool managedByAgent() const { return agentId_ > 0; }
    bool isEnabled() const { return status_ == Constants::USER_STATUS_ENABLED; }
    bool isReserved() const { return LinkHelper::isReservedAgentLink(name_, usage_); }

    // 连接状态
    const std::string& connStatus() const { return connStatus_; }
    int clientCount() const { return clientCount_; }
    const Json::Value& clients() const { return clients_; }

    /**
     * @brief 注入连接状态（由 Service 层从 TcpLinkManager 查询后设置）
     */
    void setConnectionStatus(const Json::Value& connStatus) {
        connStatus_ = connStatus.get("conn_status", "stopped").asString();
        clientCount_ = connStatus.get("client_count", 0).asInt();
        clients_ = connStatus.get("clients", Json::arrayValue);
        runtimeTargets_ = connStatus.get("targets", Json::arrayValue);
        runtimeError_ = connStatus.get("error_msg", "").asString();
        agentOnline_ = connStatus.get("agent_online", agentOnline_).asBool();
    }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["mode"] = mode_;
        json["protocol"] = protocol_;
        json["ip"] = ip_;
        json["port"] = port_;
        Json::Value targets = targets_;
        if (targets.isArray() && runtimeTargets_.isArray()) {
            for (auto& target : targets) {
                const auto targetId = target.get("id", "").asString();
                for (const auto& runtimeTarget : runtimeTargets_) {
                    if (runtimeTarget.get("id", "").asString() != targetId) continue;
                    target["conn_status"] = runtimeTarget.get("conn_status", "stopped");
                    target["error_msg"] = runtimeTarget.get("error_msg", "");
                    target["last_activity"] = runtimeTarget.get("last_activity", "");
                    break;
                }
            }
        }
        json["targets"] = std::move(targets);
        json["usage"] = usage_;
        json["is_reserved"] = isReserved();
        json["status"] = status_;
        json["created_by"] = createdBy_ > 0 ? Json::Value(createdBy_) : Json::Value::null;
        json["agent_id"] = agentId_ > 0 ? Json::Value(agentId_) : Json::nullValue;
        json["agent_interface"] = agentInterface_;
        json["agent_bind_ip"] = agentBindIp_;
        json["agent_prefix_length"] = agentPrefixLength_ > 0 ? Json::Value(agentPrefixLength_) : Json::nullValue;
        json["agent_gateway"] = agentGateway_;
        json["agent_name"] = agentName_;
        json["agent_code"] = agentCode_;
        json["agent_online"] = agentOnline_;
        json["managed_by_agent"] = managedByAgent();
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;

        // 连接状态
        json["conn_status"] = connStatus_;
        json["client_count"] = clientCount_;
        json["clients"] = clients_;
        if (!runtimeError_.empty()) {
            json["error_msg"] = runtimeError_;
        }

        return json;
    }

    // ==================== 持久化 ====================

    Task<void> doPersist(TransactionGuard& tx) {
        if (isDeleted()) {
            co_await persistDelete(tx);
        } else if (isNew()) {
            co_await persistCreate(tx);
        } else if (isDirty()) {
            co_await persistUpdate(tx);
        }
    }

private:
    // 链路基本字段
    std::string name_;
    std::string mode_;
    std::string protocol_ = Constants::PROTOCOL_SL651;
    std::string ip_;
    int port_ = 0;
    Json::Value targets_{Json::arrayValue};
    std::string usage_ = Constants::LINK_USAGE_DEVICE;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    int createdBy_ = 0;
    int agentId_ = 0;
    std::string agentInterface_;
    std::string agentBindIp_;
    int agentPrefixLength_ = 0;
    std::string agentGateway_;
    std::string agentName_;
    std::string agentCode_;
    bool agentOnline_ = false;
    std::string createdAt_;
    std::string updatedAt_;

    // 连接状态（运行时，不持久化）
    std::string connStatus_ = "stopped";
    int clientCount_ = 0;
    Json::Value clients_{Json::arrayValue};
    Json::Value runtimeTargets_{Json::arrayValue};
    std::string runtimeError_;

    // 更新标记
    bool needReload_ = false;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        mode_ = data.get("mode", "").asString();
        protocol_ = data.get("protocol", Constants::PROTOCOL_SL651).asString();
        ip_ = data.get("ip", "").asString();
        port_ = data.get("port", 0).asInt();
        targets_ = normalizeTargets(data.get("targets", Json::Value(Json::arrayValue)));
        usage_ = data.get("usage", Constants::LINK_USAGE_DEVICE).asString();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
        createdBy_ = data.get("created_by", 0).asInt();
        agentId_ = data.get("agent_id", 0).asInt();
        agentInterface_ = data.get("agent_interface", "").asString();
        agentBindIp_ = data.get("agent_bind_ip", "").asString();
        agentPrefixLength_ = data.get("agent_prefix_length", agentId_ > 0 ? 24 : 0).asInt();
        agentGateway_ = data.get("agent_gateway", "").asString();
        normalizeAgentSettings();
    }

    Task<void> load(int linkId) {
        auto result = co_await db().execSqlCoro(
            R"(
                SELECT l.*,
                       a.name as agent_name,
                       a.code as agent_code,
                       a.is_online as agent_online
                FROM link l
                LEFT JOIN agent_node a ON l.agent_id = a.id AND a.deleted_at IS NULL
                WHERE l.id = ? AND l.deleted_at IS NULL
            )",
            {std::to_string(linkId)}
        );

        if (result.empty()) {
            throw NotFoundException("链路不存在");
        }

        fromRow(result[0]);
        markLoaded();
    }

    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        mode_ = FieldHelper::getString(row["mode"]);
        protocol_ = FieldHelper::getString(row["protocol"], Constants::PROTOCOL_SL651);
        ip_ = FieldHelper::getString(row["ip"]);
        port_ = FieldHelper::getInt(row["port"]);
        targets_ = parseJsonArrayField(row["targets"]);
        usage_ = FieldHelper::getString(row["usage"], Constants::LINK_USAGE_DEVICE);
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        createdBy_ = row["created_by"].isNull() ? 0 : FieldHelper::getInt(row["created_by"]);
        agentId_ = FieldHelper::getInt(row["agent_id"]);
        agentInterface_ = FieldHelper::getString(row["agent_interface"]);
        agentBindIp_ = FieldHelper::getString(row["agent_bind_ip"]);
        agentPrefixLength_ = agentId_ > 0
            ? FieldHelper::getInt(row["agent_prefix_length"], 24)
            : 0;
        agentGateway_ = FieldHelper::getString(row["agent_gateway"]);
        agentName_ = FieldHelper::getString(row["agent_name"]);
        agentCode_ = FieldHelper::getString(row["agent_code"]);
        agentOnline_ = FieldHelper::getBool(row["agent_online"]);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
        normalizeAgentSettings();
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        // Materialize coroutine arguments before co_await. GCC/MinGW can ICE when
        // lowering the temporary initializer_list directly at the suspension point.
        std::vector<std::string> params{
            name_,
            mode_,
            protocol_,
            ip_,
            std::to_string(port_),
            JsonHelper::serialize(targets_),
            usage_,
            status_,
            std::to_string(createdBy_),
            std::to_string(agentId_),
            agentInterface_,
            agentBindIp_,
            std::to_string(agentPrefixLength_),
            agentGateway_,
            TimestampHelper::now()
        };
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO link (
                name, mode, protocol, ip, port, targets, usage, status,
                created_by, agent_id, agent_interface, agent_bind_ip, agent_prefix_length, agent_gateway, created_at
            )
            VALUES (?, ?, ?, ?, ?, ?::jsonb, ?, ?, NULLIF(?, '0')::INT, NULLIF(?, '0')::INT, ?, ?, NULLIF(?, '0')::INT, ?, ?) RETURNING id
        )", params);

        setId(FieldHelper::getInt(result[0]["id"]));

        // 事件处理器会自动启动 TCP 连接
        if (isEnabled()) {
            raiseEvent<LinkCreated>(
                id(),
                name_,
                mode_,
                protocol_,
                ip_,
                port_,
                targets_,
                agentId_,
                agentInterface_,
                agentBindIp_,
                agentPrefixLength_,
                agentGateway_
            );
        }
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        std::vector<std::string> params{
            name_,
            mode_,
            protocol_,
            ip_,
            std::to_string(port_),
            JsonHelper::serialize(targets_),
            usage_,
            status_,
            std::to_string(agentId_),
            agentInterface_,
            agentBindIp_,
            std::to_string(agentPrefixLength_),
            agentGateway_,
            TimestampHelper::now(),
            std::to_string(id())
        };
        co_await tx.execSqlCoro(R"(
            UPDATE link
            SET name = ?, mode = ?, protocol = ?, ip = ?, port = ?, targets = ?::jsonb, usage = ?, status = ?,
                agent_id = NULLIF(?, '0')::INT, agent_interface = ?, agent_bind_ip = ?,
                agent_prefix_length = NULLIF(?, '0')::INT, agent_gateway = ?, updated_at = ?
            WHERE id = ?
        )", params);

        // 事件处理器会自动重载 TCP 连接
        raiseEvent<LinkUpdated>(
            id(),
            name_,
            mode_,
            protocol_,
            ip_,
            port_,
            targets_,
            isEnabled(),
            needReload_,
            agentId_,
            agentInterface_,
            agentBindIp_,
            agentPrefixLength_,
            agentGateway_
        );
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 软删除（事件处理器会自动停止 TCP 连接）
        std::vector<std::string> params{TimestampHelper::now(), std::to_string(id())};
        co_await tx.execSqlCoro(
            "UPDATE link SET deleted_at = ? WHERE id = ?",
            params
        );

        raiseEvent<LinkDeleted>(id());
    }

    static Json::Value parseJsonField(const drogon::orm::Field& field) {
        if (field.isNull()) {
            return Json::Value(Json::objectValue);
        }

        try {
            auto parsed = JsonHelper::parse(field.as<std::string>());
            return parsed.isObject() ? parsed : Json::Value(Json::objectValue);
        } catch (...) {
            return Json::Value(Json::objectValue);
        }
    }

    static Json::Value parseJsonArrayField(const drogon::orm::Field& field) {
        if (field.isNull()) {
            return Json::Value(Json::arrayValue);
        }
        try {
            auto parsed = JsonHelper::parse(field.as<std::string>());
            return parsed.isArray() ? parsed : Json::Value(Json::arrayValue);
        } catch (...) {
            return Json::Value(Json::arrayValue);
        }
    }

    static Json::Value normalizeTargets(const Json::Value& input) {
        Json::Value normalized(Json::arrayValue);
        if (!input.isArray()) {
            return normalized;
        }
        for (const auto& source : input) {
            if (!source.isObject()) {
                normalized.append(source);
                continue;
            }
            Json::Value target(Json::objectValue);
            auto id = source.get("id", "").asString();
            if (id.empty()) {
                id = drogon::utils::getUuid();
            }
            target["id"] = id;
            target["name"] = source.get("name", "").asString();
            target["ip"] = source.get("ip", "").asString();
            target["port"] = source.get("port", 0).asInt();
            target["status"] = source.get("status", Constants::USER_STATUS_ENABLED).asString();
            normalized.append(std::move(target));
        }
        return normalized;
    }

    static std::optional<Json::Value> findAgentInterfaceByName(const Json::Value& interfaces,
                                                               const std::string& interfaceName) {
        if (!interfaces.isArray() || interfaceName.empty()) {
            return std::nullopt;
        }

        for (const auto& item : interfaces) {
            if (!item.isObject()) {
                continue;
            }
            if (item.get("name", "").asString() == interfaceName) {
                return item;
            }
        }
        return std::nullopt;
    }

    static bool isValidIpv4(const std::string& ip) {
        if (ip.empty()) {
            return false;
        }

        int parts = 0;
        int value = -1;
        for (const char ch : ip) {
            if (ch == '.') {
                if (value < 0 || value > 255) {
                    return false;
                }
                ++parts;
                value = -1;
                continue;
            }
            if (ch < '0' || ch > '9') {
                return false;
            }
            value = value < 0 ? (ch - '0') : (value * 10 + (ch - '0'));
            if (value > 255) {
                return false;
            }
        }

        return parts == 3 && value >= 0 && value <= 255;
    }

    static bool isValidPrefixLength(int prefixLength) {
        return prefixLength >= 1 && prefixLength <= 30;
    }

    void normalizeAgentSettings() {
        if (agentId_ <= 0) {
            agentId_ = 0;
            agentInterface_.clear();
            agentBindIp_.clear();
            agentPrefixLength_ = 0;
            agentGateway_.clear();
            return;
        }

        if (agentPrefixLength_ <= 0) {
            agentPrefixLength_ = 24;
        }
        if (mode_ != Constants::LINK_MODE_TCP_CLIENT) {
            agentGateway_.clear();
        }
        if (mode_ == Constants::LINK_MODE_TCP_SERVER && !agentBindIp_.empty()) {
            ip_ = agentBindIp_;
        }
    }
};
