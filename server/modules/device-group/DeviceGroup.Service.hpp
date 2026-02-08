#pragma once

#include "domain/DeviceGroup.hpp"

/**
 * @brief 设备分组服务
 */
class DeviceGroupService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 分组列表（平铺）
     */
    Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        co_return co_await DeviceGroup::list(keyword, status);
    }

    /**
     * @brief 分组树形结构
     */
    Task<Json::Value> tree(const std::string& status = "") {
        co_return co_await DeviceGroup::tree(status);
    }

    /**
     * @brief 分组树 + 每个节点的设备数量
     */
    Task<Json::Value> treeWithCount() {
        auto tree = co_await DeviceGroup::tree();

        // 查询每个分组的设备数量
        DatabaseService db;
        auto countResult = co_await db.execSqlCoro(R"(
            SELECT group_id, COUNT(*) as device_count
            FROM device
            WHERE deleted_at IS NULL AND group_id IS NOT NULL
            GROUP BY group_id
        )");

        // 构建 group_id -> count 映射
        std::map<int, int> countMap;
        for (const auto& row : countResult) {
            int groupId = FieldHelper::getInt(row["group_id"]);
            int count = FieldHelper::getInt(row["device_count"]);
            countMap[groupId] = count;
        }

        // 递归填充 deviceCount
        std::function<void(Json::Value&)> fillCount = [&](Json::Value& nodes) {
            for (auto& node : nodes) {
                int nodeId = node["id"].asInt();
                node["deviceCount"] = countMap.count(nodeId) ? countMap[nodeId] : 0;
                if (node.isMember("children") && node["children"].isArray()) {
                    fillCount(node["children"]);
                }
            }
        };
        fillCount(tree);

        co_return tree;
    }

    /**
     * @brief 分组详情
     */
    Task<Json::Value> detail(int id) {
        auto group = co_await DeviceGroup::of(id);
        co_return group.toJson();
    }

    /**
     * @brief 创建分组
     */
    Task<void> create(const Json::Value& data) {
        co_await DeviceGroup::create(data)
            .require(DeviceGroup::nameUnique)
            .requireIf(data.get("parent_id", 0).asInt() > 0, DeviceGroup::parentExists)
            .save();
    }

    /**
     * @brief 更新分组
     */
    Task<void> update(int id, const Json::Value& data) {
        auto group = co_await DeviceGroup::of(id);

        bool parentChanged = data.isMember("parent_id") && !data["parent_id"].isNull();

        group.require(DeviceGroup::nameUnique)
            .requireIf(parentChanged, DeviceGroup::parentExists)
            .requireIf(parentChanged, DeviceGroup::notSelfParent)
            .update(data);

        co_await group.save();
    }

    /**
     * @brief 删除分组
     */
    Task<void> remove(int id) {
        auto group = co_await DeviceGroup::of(id);
        co_await group.require(DeviceGroup::noChildren)
            .require(DeviceGroup::noDevices)
            .remove()
            .save();
    }
};
