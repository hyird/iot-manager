#pragma once

#include "domain/ProtocolConfig.hpp"
#include "ProtocolConfigValidator.hpp"
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
        ProtocolConfigValidator::requireSupportedProtocol(protocol);

        co_return co_await ProtocolConfig::options(protocol);
    }

    /**
     * @brief 创建配置
     */
    Task<void> create(const Json::Value& data) {
        auto config = ProtocolConfig::create(data);
        ProtocolConfigValidator::requireSupportedProtocol(config.protocol());

        if (data.isMember("config") && data["config"].isObject()) {
            ProtocolConfigValidator::validate(config.protocol(), data["config"]);
        }

        config.require(ProtocolConfig::protocolRequired)
              .require(ProtocolConfig::nameRequired)
              .require(ProtocolConfig::nameUnique);

        co_await config.save();
    }

    /**
     * @brief 更新配置
     */
    Task<void> update(int id, const Json::Value& data) {
        auto config = co_await ProtocolConfig::of(id);

        // 先应用更新，再检查约束（nameUnique 需要检查更新后的名称）
        config.update(data);

        if (data.isMember("protocol")) {
            std::string requestedProtocol = data["protocol"].asString();
            if (!requestedProtocol.empty() && requestedProtocol != config.protocol()) {
                throw ConflictException("协议类型不可修改");
            }
        }

        // 使用已加载的 protocol 校验 config 结构（即使请求体未携带 protocol 也能校验）
        if (data.isMember("config") && data["config"].isObject()) {
            ProtocolConfigValidator::validate(config.protocol(), data["config"]);
        }

        if (data.isMember("name")) {
            config.require(ProtocolConfig::nameUnique);
        }

        co_await config.save();
    }

    /**
     * @brief 删除配置
     */
    Task<void> remove(int id) {
        auto config = co_await ProtocolConfig::of(id);
        co_await config.require(ProtocolConfig::noDevices)
            .remove()
            .save();
    }
};
