#pragma once

#include <drogon/drogon.h>
#include <json/json.h>
#include <vector>
#include "common/domain/Aggregate.hpp"
#include "common/domain/DomainEvent.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/Pagination.hpp"
#include "common/network/TcpLinkManager.hpp"

using namespace drogon;

/**
 * @brief 链路聚合根
 *
 * 管理 TCP Server/Client 链路配置，与 TcpLinkManager 集成自动管理连接。
 *
 * 使用示例：
 * @code
 * // 创建链路（自动启动 TCP 连接）
 * co_await Link::create(data)
 *     .require(Link::nameUnique)
 *     .require(Link::endpointUnique)
 *     .save();
 *
 * // 更新链路（配置变更时自动重载连接）
 * co_await Link::of(id)
 *     .update(data)
 *     .save();
 *
 * // 删除链路（自动停止 TCP 连接）
 * co_await Link::of(id)
 *     .remove()
 *     .save();
 * @endcode
 */
class Link : public Aggregate<Link> {
public:
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
            link.loadConnectionStatus();
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
            "SELECT id, name, mode, ip, port FROM link WHERE deleted_at IS NULL ORDER BY name ASC"
        );

        Json::Value items(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = FieldHelper::getInt(row["id"]);
            item["name"] = FieldHelper::getString(row["name"]);
            item["mode"] = FieldHelper::getString(row["mode"]);
            item["ip"] = FieldHelper::getString(row["ip"]);
            item["port"] = FieldHelper::getInt(row["port"]);
            items.append(item);
        }
        co_return items;
    }

    /**
     * @brief 启动所有已启用的链路（服务器启动时调用）
     */
    static Task<void> startAllEnabled() {
        DatabaseService db;
        auto result = co_await db.execSqlCoro(
            "SELECT * FROM link WHERE status = 'enabled' AND deleted_at IS NULL"
        );

        for (const auto& row : result) {
            int linkId = FieldHelper::getInt(row["id"]);
            std::string name = FieldHelper::getString(row["name"]);
            std::string mode = FieldHelper::getString(row["mode"]);
            std::string ip = FieldHelper::getString(row["ip"]);
            int port = FieldHelper::getInt(row["port"]);

            try {
                startTcpLink(linkId, name, mode, ip, static_cast<uint16_t>(port));
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to start link " << linkId << ": " << e.what();
            }
        }

        LOG_INFO << "Started " << result.size() << " enabled links";
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

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新链路信息
     */
    Link& update(const Json::Value& data) {
        if (data.isMember("name")) {
            name_ = data["name"].asString();
            markDirty();
        }
        if (data.isMember("mode")) {
            mode_ = data["mode"].asString();
            markDirty();
            needReload_ = true;
        }
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
    const std::string& ip() const { return ip_; }
    int port() const { return port_; }
    const std::string& status() const { return status_; }
    bool isEnabled() const { return status_ == "enabled"; }

    // 连接状态
    const std::string& connStatus() const { return connStatus_; }
    int clientCount() const { return clientCount_; }
    const Json::Value& clients() const { return clients_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["name"] = name_;
        json["mode"] = mode_;
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
    std::string ip_;
    int port_ = 0;
    std::string status_ = "enabled";
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
        ip_ = data.get("ip", "").asString();
        port_ = data.get("port", 0).asInt();
        status_ = data.get("status", "enabled").asString();
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
        loadConnectionStatus();
        markLoaded();
    }

    void fromRow(const drogon::orm::Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        name_ = FieldHelper::getString(row["name"]);
        mode_ = FieldHelper::getString(row["mode"]);
        ip_ = FieldHelper::getString(row["ip"]);
        port_ = FieldHelper::getInt(row["port"]);
        status_ = FieldHelper::getString(row["status"], "enabled");
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");
    }

    void loadConnectionStatus() {
        auto connStatus = TcpLinkManager::instance().getStatus(id());
        connStatus_ = connStatus.get("conn_status", "stopped").asString();
        clientCount_ = connStatus.get("client_count", 0).asInt();
        clients_ = connStatus.get("clients", Json::arrayValue);
    }

    // ==================== TCP 连接管理 ====================

    static void startTcpLink(int linkId, const std::string& name, const std::string& mode,
                             const std::string& ip, uint16_t port) {
        if (mode == "TCP Server") {
            TcpLinkManager::instance().startServer(linkId, name, ip, port);
        } else if (mode == "TCP Client") {
            TcpLinkManager::instance().startClient(linkId, name, ip, port);
        } else {
            throw ValidationException("不支持的链路模式: " + mode);
        }
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO link (name, mode, ip, port, status, created_at)
            VALUES (?, ?, ?, ?, ?, ?) RETURNING id
        )", {
            name_, mode_, ip_, std::to_string(port_), status_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));

        // 自动启动 TCP 连接
        if (isEnabled()) {
            startTcpLink(id(), name_, mode_, ip_, static_cast<uint16_t>(port_));
        }

        raiseEvent<LinkCreated>(id(), name_, mode_, ip_, port_);
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE link
            SET name = ?, mode = ?, ip = ?, port = ?, status = ?, updated_at = ?
            WHERE id = ?
        )", {
            name_, mode_, ip_, std::to_string(port_), status_,
            TimestampHelper::now(), std::to_string(id())
        });

        // 重载 TCP 连接
        if (needReload_) {
            TcpLinkManager::instance().reload(
                id(), name_, mode_, ip_,
                static_cast<uint16_t>(port_),
                isEnabled()
            );
        }

        raiseEvent<LinkUpdated>(id(), name_, mode_, ip_, port_, isEnabled(), needReload_);
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 先停止 TCP 连接
        TcpLinkManager::instance().stop(id());

        // 软删除
        co_await tx.execSqlCoro(
            "UPDATE link SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<LinkDeleted>(id());
    }
};
