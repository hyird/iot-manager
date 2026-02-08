#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/link/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"

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
        qb.notDeleted();
        if (!page.keyword.empty()) qb.likeAny({"name", "ip"}, page.keyword);
        if (!mode.empty()) qb.eq("mode", mode);

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM link" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据
        std::string sql = "SELECT * FROM link" + qb.whereClause()
                        + " ORDER BY id ASC" + page.limitClause();
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
            "SELECT id, name, mode, protocol, ip, port FROM link WHERE deleted_at IS NULL ORDER BY name ASC"
        );

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"]);
            item["mode"] = FieldHelper::getString(row["mode"]);
            item["protocol"] = FieldHelper::getString(row["protocol"], Constants::PROTOCOL_SL651);
            item["ip"] = FieldHelper::getString(row["ip"]);
            item["port"] = FieldHelper::getInt(row["port"]);
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
            "SELECT * FROM link WHERE status = 'enabled' AND deleted_at IS NULL"
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
            throw ValidationException("链路名称已存在");
        }
    }

    /**
     * @brief 约束：端点（mode + ip + port）唯一
     */
    static Task<void> endpointUnique(const Link& link) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM link WHERE mode = ? AND ip = ? AND port = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {link.mode_, link.ip_, std::to_string(link.port_)};

        if (link.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(link.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("相同模式、IP和端口的链路已存在");
        }
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
            throw ValidationException("该链路下存在关联设备，无法删除");
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
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
            needReload_ = true;
        }
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
    const std::string& status() const { return status_; }
    bool isEnabled() const { return status_ == Constants::USER_STATUS_ENABLED; }

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
        json["status"] = status_;
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;

        // 连接状态
        json["conn_status"] = connStatus_;
        json["client_count"] = clientCount_;
        json["clients"] = clients_;

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
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string createdAt_;
    std::string updatedAt_;

    // 连接状态（运行时，不持久化）
    std::string connStatus_ = "stopped";
    int clientCount_ = 0;
    Json::Value clients_{Json::arrayValue};

    // 更新标记
    bool needReload_ = false;

    void applyCreate(const Json::Value& data) {
        name_ = data.get("name", "").asString();
        mode_ = data.get("mode", "").asString();
        protocol_ = data.get("protocol", Constants::PROTOCOL_SL651).asString();
        ip_ = data.get("ip", "").asString();
        port_ = data.get("port", 0).asInt();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
    }

    Task<void> load(int linkId) {
        auto result = co_await db().execSqlCoro(
            "SELECT * FROM link WHERE id = ? AND deleted_at IS NULL",
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
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO link (name, mode, protocol, ip, port, status, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id
        )", {
            name_, mode_, protocol_, ip_, std::to_string(port_), status_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));

        // 事件处理器会自动启动 TCP 连接
        if (isEnabled()) {
            raiseEvent<LinkCreated>(id(), name_, mode_, ip_, port_);
        }
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE link
            SET name = ?, mode = ?, protocol = ?, ip = ?, port = ?, status = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, mode_, protocol_, ip_, std::to_string(port_), status_,
            TimestampHelper::now(), std::to_string(id())
        });

        // 事件处理器会自动重载 TCP 连接
        raiseEvent<LinkUpdated>(id(), name_, mode_, ip_, port_, isEnabled(), needReload_);
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 软删除（事件处理器会自动停止 TCP 连接）
        co_await tx.execSqlCoro(
            "UPDATE link SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<LinkDeleted>(id());
    }
};
