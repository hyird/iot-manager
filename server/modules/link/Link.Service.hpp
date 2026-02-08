#pragma once

#include "domain/Link.hpp"
#include "domain/LinkEventHandlers.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 链路服务
 *
 * 负责：
 * - CRUD 操作（委托给 Link 聚合根）
 * - 连接状态查询（从 TcpLinkManager 注入到 Link）
 * - 批量启动（服务器启动时调用）
 *
 * TCP 连接的启停由事件驱动（LinkEventHandlers），Service 只负责查询状态。
 */
class LinkService {
public:
    template<typename T = void> using Task = drogon::Task<T>;
    /**
     * @brief 链路列表（分页，包含连接状态）
     */
    Task<std::tuple<Json::Value, int>> list(const Pagination& page, const std::string& mode = "") {
        auto result = co_await Link::list(page, mode);

        Json::Value items(Json::arrayValue);
        for (auto& link : result.items) {
            injectConnectionStatus(link);
            items.append(link.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 链路详情（包含连接状态）
     */
    Task<Json::Value> detail(int id) {
        auto link = co_await Link::of(id);
        injectConnectionStatus(link);
        co_return link.toJson();
    }

    /**
     * @brief 链路选项（下拉选择用）
     */
    Task<Json::Value> options() {
        co_return co_await Link::options();
    }

    /**
     * @brief 创建链路（事件处理器自动启动 TCP 连接）
     */
    Task<void> create(const Json::Value& data) {
        auto link = Link::create(data);

        link.require(Link::nameUnique)
            .require(Link::endpointUnique);

        co_await link.save();
    }

    /**
     * @brief 更新链路（事件处理器自动重载连接）
     */
    Task<void> update(int id, const Json::Value& data) {
        auto link = co_await Link::of(id);

        // 根据修改的字段添加约束
        if (data.isMember("name")) {
            link.require(Link::nameUnique);
        }
        if (data.isMember("mode") || data.isMember("ip") || data.isMember("port")) {
            link.require(Link::endpointUnique);
        }

        link.update(data);

        co_await link.save();
    }

    /**
     * @brief 删除链路（事件处理器自动停止 TCP 连接）
     */
    Task<void> remove(int id) {
        auto link = co_await Link::of(id);
        co_await link.require(Link::noDevices)
            .remove()
            .save();
    }

    /**
     * @brief 启动所有已启用的链路（服务器启动时调用）
     */
    Task<void> startAllEnabled() {
        auto links = co_await Link::findAllEnabled();

        for (const auto& link : links) {
            try {
                auto& mgr = TcpLinkManager::instance();
                auto port = static_cast<uint16_t>(link.port());
                if (link.mode() == Constants::LINK_MODE_TCP_SERVER) {
                    mgr.startServer(link.id(), link.name(), link.ip(), port);
                } else if (link.mode() == Constants::LINK_MODE_TCP_CLIENT) {
                    mgr.startClient(link.id(), link.name(), link.ip(), port);
                }
            } catch (const std::exception& e) {
                LOG_ERROR << "Failed to start link " << link.id() << ": " << e.what();
            }
        }

        LOG_INFO << "Started " << links.size() << " enabled links";
    }

private:
    /**
     * @brief 从 TcpLinkManager 查询连接状态并注入到 Link
     */
    void injectConnectionStatus(Link& link) {
        auto status = TcpLinkManager::instance().getStatus(link.id());
        link.setConnectionStatus(status);
    }
};
