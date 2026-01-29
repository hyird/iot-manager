#pragma once

#include "domain/Link.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 链路服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~302 行
 * - 改进后：~70 行
 *
 * TCP 连接管理由 Link 领域模型自动处理。
 */
class LinkService {
public:
    /**
     * @brief 链路列表（分页，包含连接状态）
     */
    Task<std::tuple<Json::Value, int>> list(const Pagination& page, const std::string& mode = "") {
        auto result = co_await Link::list(page, mode);

        Json::Value items(Json::arrayValue);
        for (const auto& link : result.items) {
            items.append(link.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 链路详情（包含连接状态）
     */
    Task<Json::Value> detail(int id) {
        auto link = co_await Link::of(id);
        co_return link.toJson();
    }

    /**
     * @brief 链路选项（下拉选择用）
     */
    Task<Json::Value> options() {
        co_return co_await Link::options();
    }

    /**
     * @brief 创建链路（自动启动 TCP 连接）
     */
    Task<Json::Value> create(const Json::Value& data) {
        auto link = Link::create(data);

        link.require(Link::nameUnique)
            .require(Link::endpointUnique);

        co_await link.save();

        // 返回创建后的详情
        co_return co_await detail(link.id());
    }

    /**
     * @brief 更新链路（配置变更时自动重载连接）
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
     * @brief 删除链路（自动停止 TCP 连接）
     */
    Task<void> remove(int id) {
        auto link = co_await Link::of(id);
        co_await link.remove()
            .save();
    }

    /**
     * @brief 启动所有已启用的链路（服务器启动时调用）
     */
    Task<void> startAllEnabled() {
        co_await Link::startAllEnabled();
    }
};
