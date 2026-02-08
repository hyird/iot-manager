#pragma once

#include "DomainEvent.hpp"
#include "EventBus.hpp"
#include "common/database/DatabaseService.hpp"
#include "common/database/TransactionGuard.hpp"
#include "common/utils/AppException.hpp"

/**
 * @brief 约束检查函数类型
 */
template<typename T>
using Constraint = std::function<drogon::Task<void>(const T&)>;

/**
 * @brief 聚合根基类
 *
 * 所有领域模型的基础，提供：
 * - 流式 API（链式调用）
 * - 声明式约束检查
 * - 自动事务管理
 * - 自动事件发布
 *
 * 使用示例：
 * @code
 * co_await User::of(id)
 *     .require(User::notBuiltinAdmin)
 *     .update(data)
 *     .withRoles(roleIds)
 *     .save();
 * @endcode
 */
template<typename Derived>
class Aggregate {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;

    // ========== 状态查询 ==========

    int id() const { return id_; }
    bool isNew() const { return id_ == 0; }
    bool isLoaded() const { return loaded_; }
    bool isDirty() const { return dirty_; }
    bool isDeleted() const { return deleted_; }

    // ========== 流式 API ==========

    /**
     * @brief 添加约束检查（声明式）
     *
     * 约束在 save() 时自动执行，失败则抛出异常阻止保存。
     *
     * @code
     * user.require(User::notBuiltinAdmin)
     *     .require(User::usernameUnique);
     * @endcode
     */
    Derived& require(Constraint<Derived> constraint) {
        constraints_.push_back(std::move(constraint));
        return self();
    }

    /**
     * @brief 条件约束
     */
    Derived& requireIf(bool condition, Constraint<Derived> constraint) {
        if (condition) {
            constraints_.push_back(std::move(constraint));
        }
        return self();
    }

    /**
     * @brief 保存聚合根
     *
     * 执行流程：
     * 1. 执行所有约束检查
     * 2. 开启事务
     * 3. 持久化数据（由子类实现 doPersist）
     * 4. 提交事务
     * 5. 发布领域事件
     */
    Task<void> save() {
        // 1. 执行约束检查
        for (const auto& constraint : constraints_) {
            co_await constraint(self());
        }
        constraints_.clear();

        // 2. 开启事务并持久化
        auto tx = co_await TransactionGuard::create(dbService_);

        co_await self().doPersist(tx);

        co_await tx.commit();

        // 3. 发布领域事件
        for (auto& event : pendingEvents_) {
            co_await EventBus::instance().publish(*event);
        }
        pendingEvents_.clear();

        // 4. 重置状态
        dirty_ = false;
        deleted_ = false;
    }

    /**
     * @brief 保存并返回自身（用于链式调用后获取结果）
     */
    Task<Derived> saveAndReturn() {
        co_await save();
        co_return self();
    }

protected:
    int id_ = 0;
    bool loaded_ = false;
    bool dirty_ = false;
    bool deleted_ = false;
    DatabaseService dbService_;
    std::vector<Constraint<Derived>> constraints_;
    std::vector<std::unique_ptr<DomainEvent>> pendingEvents_;

    Aggregate() = default;

    // ========== 子类辅助方法 ==========

    /**
     * @brief 标记为脏（有修改）
     */
    void markDirty() { dirty_ = true; }

    /**
     * @brief 标记为已删除
     */
    void markDeleted() { deleted_ = true; }

    /**
     * @brief 标记为已加载
     */
    void markLoaded() { loaded_ = true; }

    /**
     * @brief 设置 ID
     */
    void setId(int id) { id_ = id; }

    /**
     * @brief 添加待发布的领域事件
     */
    template<typename E, typename... Args>
    void raiseEvent(Args&&... args) {
        static_assert(std::is_base_of_v<DomainEvent, E>, "E must derive from DomainEvent");
        pendingEvents_.push_back(std::make_unique<E>(std::forward<Args>(args)...));
    }

    /**
     * @brief 获取数据库服务
     */
    DatabaseService& db() { return dbService_; }

private:
    Derived& self() { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }
};

/**
 * @brief 分页查询结果
 */
template<typename T>
struct PagedResult {
    std::vector<T> items;
    int total;
    int page;
    int pageSize;

    Json::Value toJson() const {
        Json::Value data;
        Json::Value list(Json::arrayValue);
        for (const auto& item : items) {
            list.append(item.toJson());
        }
        data["list"] = list;
        data["total"] = total;
        if (pageSize > 0) {
            data["page"] = page;
            data["pageSize"] = pageSize;
            data["totalPages"] = static_cast<int>(std::ceil(static_cast<double>(total) / pageSize));
        }
        return data;
    }
};
