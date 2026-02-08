#pragma once

#include "domain/Department.hpp"

/**
 * @brief 部门服务（领域驱动重构版）
 *
 * 对比改进前：
 * - 改进前：~156 行
 * - 改进后：~45 行
 */
class DepartmentService {
public:
    template<typename T = void> using Task = drogon::Task<T>;

    /**
     * @brief 部门列表（平铺）
     */
    Task<Json::Value> list(const std::string& keyword = "", const std::string& status = "") {
        co_return co_await Department::list(keyword, status);
    }

    /**
     * @brief 部门树形结构
     */
    Task<Json::Value> tree(const std::string& status = "") {
        co_return co_await Department::tree(status);
    }

    /**
     * @brief 部门详情
     */
    Task<Json::Value> detail(int id) {
        auto dept = co_await Department::of(id);
        co_return dept.toJson();
    }

    /**
     * @brief 创建部门
     */
    Task<void> create(const Json::Value& data) {
        std::string code = data.get("code", "").asString();

        co_await Department::create(data)
            .requireIf(!code.empty(), Department::codeUnique)
            .save();
    }

    /**
     * @brief 更新部门
     */
    Task<void> update(int id, const Json::Value& data) {
        auto dept = co_await Department::of(id);

        bool codeChanged = data.isMember("code") && !data["code"].isNull()
                        && !data["code"].asString().empty();
        bool parentChanged = data.isMember("parent_id");

        dept.requireIf(codeChanged, Department::codeUnique);
        if (parentChanged) {
            int newParentId = (data["parent_id"].isNull() || data["parent_id"].asInt() == 0)
                ? 0 : data["parent_id"].asInt();
            dept.require(Department::notCircularParent(newParentId));
        }
        dept.update(data);

        co_await dept.save();
    }

    /**
     * @brief 删除部门
     */
    Task<void> remove(int id) {
        auto dept = co_await Department::of(id);
        co_await dept.require(Department::noChildren)
            .require(Department::noUsers)
            .remove()
            .save();
    }
};
