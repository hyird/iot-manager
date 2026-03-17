#pragma once

#include "MigrationTypes.hpp"
#include <algorithm>
#include <functional>

/**
 * @brief 迁移注册中心
 *
 * 管理所有版本化迁移实例，按版本号排序。
 * 使用显式注册方式，不使用宏或静态初始化黑魔法。
 *
 * 使用示例：
 * @code
 * MigrationRegistry registry;
 * registry.add<V001_Baseline>();
 * registry.add<V002_AddDeviceTags>();
 * // MigrationRunner 使用 registry.getMigrations() 获取有序迁移列表
 * @endcode
 */
class MigrationRegistry {
public:
    MigrationRegistry() = default;

    /// 注册一个迁移（模板便捷方法）
    template<typename T, typename... Args>
    void add(Args&&... args) {
        auto migration = std::make_unique<T>(std::forward<Args>(args)...);
        addImpl(std::move(migration));
    }

    /// 注册一个迁移（指针方式）
    void add(std::unique_ptr<MigrationBase> migration) {
        addImpl(std::move(migration));
    }

    /// 获取所有迁移（按版本号升序）
    const std::vector<std::unique_ptr<MigrationBase>>& getMigrations() const {
        return migrations_;
    }

    /// 获取最新版本号（无迁移时返回 0）
    int getLatestVersion() const {
        if (migrations_.empty()) return 0;
        return migrations_.back()->info().version;
    }

    /// 获取迁移数量
    size_t size() const { return migrations_.size(); }

    /// 按版本号查找迁移（未找到返回 nullptr）
    const MigrationBase* findByVersion(int version) const {
        for (const auto& m : migrations_) {
            if (m->info().version == version) return m.get();
        }
        return nullptr;
    }

    /// 校验版本号连续性（从 1 开始，无跳号）
    void validate() const {
        for (size_t i = 0; i < migrations_.size(); ++i) {
            int expected = static_cast<int>(i) + 1;
            int actual = migrations_[i]->info().version;
            if (actual != expected) {
                throw std::runtime_error(
                    "Migration version gap: expected V" +
                    std::to_string(expected) + " but got V" +
                    std::to_string(actual) +
                    ". Migrations must be consecutively numbered starting from 1.");
            }
        }
    }

private:
    std::vector<std::unique_ptr<MigrationBase>> migrations_;

    void addImpl(std::unique_ptr<MigrationBase> migration) {
        int ver = migration->info().version;

        // 检查版本号重复
        for (const auto& m : migrations_) {
            if (m->info().version == ver) {
                throw std::runtime_error(
                    "Duplicate migration version: V" + std::to_string(ver));
            }
        }

        migrations_.push_back(std::move(migration));

        // 按版本号排序
        std::sort(migrations_.begin(), migrations_.end(),
            [](const std::unique_ptr<MigrationBase>& a,
               const std::unique_ptr<MigrationBase>& b) {
                return a->info().version < b->info().version;
            });
    }
};
