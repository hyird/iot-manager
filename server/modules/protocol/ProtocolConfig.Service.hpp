#pragma once

#include "domain/ProtocolConfig.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 协议配置服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~221 行
 * - 改进后：~65 行
 */
class ProtocolConfigService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 配置列表（分页）
     */
    Task<std::tuple<Json::Value, int>> list(const Pagination& page, const std::string& protocol = "") {
        auto result = co_await ProtocolConfig::list(page, protocol);

        Json::Value items(Json::arrayValue);
        for (const auto& config : result.items) {
            items.append(config.toJson());
        }

        co_return std::make_tuple(items, result.total);
    }

    /**
     * @brief 配置详情
     */
    Task<Json::Value> detail(int id) {
        auto config = co_await ProtocolConfig::of(id);
        co_return config.toJson();
    }

    /**
     * @brief 配置选项（下拉选择用）
     */
    Task<Json::Value> options(const std::string& protocol) {
        co_return co_await ProtocolConfig::options(protocol);
    }

    /**
     * @brief 创建配置
     */
    Task<Json::Value> create(const Json::Value& data) {
        auto config = ProtocolConfig::create(data);

        config.require(ProtocolConfig::protocolRequired)
              .require(ProtocolConfig::nameRequired)
              .require(ProtocolConfig::nameUnique);

        co_await config.save();

        // 返回创建后的详情
        co_return co_await detail(config.id());
    }

    /**
     * @brief 更新配置
     */
    Task<void> update(int id, const Json::Value& data) {
        auto config = co_await ProtocolConfig::of(id);

        // 如果修改了名称，需要检查唯一性
        if (data.isMember("name")) {
            config.require(ProtocolConfig::nameUnique);
        }

        config.update(data);

        co_await config.save();
    }

    /**
     * @brief 删除配置
     */
    Task<void> remove(int id) {
        auto config = co_await ProtocolConfig::of(id);
        co_await config.remove()
            .save();
    }
};
