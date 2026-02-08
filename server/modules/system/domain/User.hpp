#pragma once

#include "common/domain/Aggregate.hpp"
#include "modules/system/domain/Events.hpp"
#include "common/utils/Constants.hpp"
#include "common/utils/PasswordUtils.hpp"
#include "common/utils/FieldHelper.hpp"
#include "common/utils/TimestampHelper.hpp"
#include "common/utils/SqlHelper.hpp"
#include "common/utils/Pagination.hpp"

/**
 * @brief 用户聚合根
 *
 * 领域驱动设计的核心实体，封装用户相关的所有业务逻辑。
 *
 * 使用示例：
 * @code
 * // 创建用户
 * co_await User::create(data)
 *     .require(User::usernameUnique)
 *     .withRoles({1, 2, 3})
 *     .save();
 *
 * // 更新用户
 * co_await User::of(id)
 *     .require(User::notBuiltinAdmin)
 *     .update(data)
 *     .withRoles(data["roleIds"])
 *     .save();
 *
 * // 删除用户
 * co_await User::of(id)
 *     .require(User::notBuiltinAdmin)
 *     .remove()
 *     .save();
 * @endcode
 */
class User : public Aggregate<User> {
public:
    template<typename T = void>
    using Task = drogon::Task<T>;
    using Row = drogon::orm::Row;

    // ==================== 静态工厂方法 ====================

    /**
     * @brief 加载已存在的用户
     */
    static Task<User> of(int id) {
        User user;
        co_await user.load(id);
        co_return user;
    }

    /**
     * @brief 创建新用户
     */
    static User create(const Json::Value& data) {
        User user;
        user.applyCreate(data);
        return user;
    }

    /**
     * @brief 分页查询用户列表
     */
    static Task<PagedResult<User>> list(
        const Pagination& page,
        const std::string& status = "",
        int departmentId = 0
    ) {
        DatabaseService db;

        QueryBuilder qb;
        qb.notDeleted("u.deleted_at");
        if (!page.keyword.empty()) {
            qb.likeAny({"u.username", "u.nickname", "u.phone", "u.email"}, page.keyword);
        }
        if (!status.empty()) qb.eq("u.status", status);
        if (departmentId > 0) qb.eq("u.department_id", std::to_string(departmentId));

        // 计数
        auto countResult = co_await db.execSqlCoro(
            "SELECT COUNT(*) as count FROM sys_user u" + qb.whereClause(),
            qb.params()
        );
        int total = countResult.empty() ? 0 : FieldHelper::getInt(countResult[0]["count"]);

        // 查询数据
        std::string sql = R"(
            SELECT u.*, d.name as department_name
            FROM sys_user u
            LEFT JOIN sys_department d ON u.department_id = d.id AND d.deleted_at IS NULL
        )" + qb.whereClause() + " ORDER BY u.id ASC" + page.limitClause();

        auto result = co_await db.execSqlCoro(sql, qb.params());

        // 收集用户 ID，批量查询角色
        std::vector<int> userIds;
        userIds.reserve(result.size());
        for (const auto& row : result) {
            userIds.push_back(FieldHelper::getInt(row["id"]));
        }

        auto rolesMap = co_await batchLoadRoles(userIds);

        // 构建结果
        std::vector<User> users;
        users.reserve(result.size());
        for (const auto& row : result) {
            User user;
            user.fromRow(row);
            int userId = user.id();
            auto it = rolesMap.find(userId);
            if (it != rolesMap.end()) {
                user.roles_ = it->second;
            }
            users.push_back(std::move(user));
        }

        co_return PagedResult<User>{std::move(users), total, page.page, page.pageSize};
    }

    // ==================== 声明式约束 ====================

    /**
     * @brief 约束：不能是内置管理员
     */
    static Task<void> notBuiltinAdmin(const User& user) {
        if (user.username_ == "admin") {
            throw ForbiddenException("不能编辑或删除内建管理员账户");
        }
        co_return;
    }

    /**
     * @brief 约束：用户名唯一
     */
    static Task<void> usernameUnique(const User& user) {
        DatabaseService db;
        std::string sql = "SELECT 1 FROM sys_user WHERE username = ? AND deleted_at IS NULL";
        std::vector<std::string> params = {user.username_};

        if (user.id() > 0) {
            sql += " AND id != ?";
            params.push_back(std::to_string(user.id()));
        }

        auto result = co_await db.execSqlCoro(sql, params);
        if (!result.empty()) {
            throw ValidationException("用户名已存在");
        }
    }

    /**
     * @brief 约束：不能删除自己
     */
    static Constraint<User> notSelf(int currentUserId) {
        return [currentUserId](const User& user) -> Task<void> {
            if (user.id() == currentUserId) {
                throw ValidationException("不能删除当前登录用户");
            }
            co_return;
        };
    }

    // ==================== 业务操作（流式 API）====================

    /**
     * @brief 更新用户信息
     */
    User& update(const Json::Value& data) {
        if (data.isMember("nickname")) {
            nickname_ = data["nickname"].asString();
            markDirty();
        }
        if (data.isMember("phone")) {
            phone_ = data["phone"].asString();
            markDirty();
        }
        if (data.isMember("email")) {
            email_ = data["email"].asString();
            markDirty();
        }
        if (data.isMember("status")) {
            status_ = data["status"].asString();
            markDirty();
        }
        if (data.isMember("departmentId")) {
            departmentId_ = data["departmentId"].isNull() ? 0 : data["departmentId"].asInt();
            markDirty();
        }
        if (data.isMember("password") && !data["password"].asString().empty()) {
            passwordHash_ = PasswordUtils::hashPassword(data["password"].asString());
            markDirty();
        }
        return *this;
    }

    /**
     * @brief 设置用户角色
     */
    User& withRoles(const Json::Value& roleIds) {
        pendingRoleIds_.clear();
        if (roleIds.isArray()) {
            for (const auto& rid : roleIds) {
                pendingRoleIds_.push_back(rid.asInt());
            }
        }
        rolesChanged_ = true;
        return *this;
    }

    /**
     * @brief 设置用户角色（vector 版本）
     */
    User& withRoles(const std::vector<int>& roleIds) {
        pendingRoleIds_ = roleIds;
        rolesChanged_ = true;
        return *this;
    }

    /**
     * @brief 标记删除
     */
    User& remove() {
        markDeleted();
        return *this;
    }

    // ==================== 数据访问 ====================

    const std::string& username() const { return username_; }
    const std::string& nickname() const { return nickname_; }
    const std::string& phone() const { return phone_; }
    const std::string& email() const { return email_; }
    const std::string& status() const { return status_; }
    int departmentId() const { return departmentId_; }
    const std::string& departmentName() const { return departmentName_; }
    const Json::Value& roles() const { return roles_; }
    const std::vector<int>& roleIds() const { return roleIds_; }

    /**
     * @brief 转换为 JSON
     */
    Json::Value toJson() const {
        Json::Value json;
        json["id"] = id();
        json["username"] = username_;
        json["nickname"] = nickname_;
        json["phone"] = phone_;
        json["email"] = email_;
        json["department_id"] = departmentId_;
        json["department_name"] = departmentName_;
        json["status"] = status_;
        json["created_at"] = createdAt_;
        json["updated_at"] = updatedAt_;
        json["roles"] = roles_;

        Json::Value ids(Json::arrayValue);
        for (int rid : roleIds_) {
            ids.append(rid);
        }
        json["roleIds"] = ids;

        return json;
    }

    // ==================== 持久化（由 Aggregate 基类调用）====================

    Task<void> doPersist(TransactionGuard& tx) {
        if (isDeleted()) {
            co_await persistDelete(tx);
        } else if (isNew()) {
            co_await persistCreate(tx);
        } else if (isDirty()) {
            co_await persistUpdate(tx);
        }

        if (rolesChanged_) {
            co_await persistRoles(tx);
        }
    }

private:
    // 用户字段
    std::string username_;
    std::string passwordHash_;
    std::string nickname_;
    std::string phone_;
    std::string email_;
    int departmentId_ = 0;
    std::string departmentName_;
    std::string status_ = Constants::USER_STATUS_ENABLED;
    std::string createdAt_;
    std::string updatedAt_;

    // 关联数据
    std::vector<int> roleIds_;
    Json::Value roles_{Json::arrayValue};

    // 待保存的角色
    std::vector<int> pendingRoleIds_;
    bool rolesChanged_ = false;

    // ==================== 私有方法 ====================

    /**
     * @brief 从请求数据创建
     */
    void applyCreate(const Json::Value& data) {
        username_ = data.get("username", "").asString();
        passwordHash_ = PasswordUtils::hashPassword(data.get("password", "").asString());
        nickname_ = data.get("nickname", "").asString();
        phone_ = data.get("phone", "").asString();
        email_ = data.get("email", "").asString();
        departmentId_ = data.get("departmentId", 0).asInt();
        status_ = data.get("status", Constants::USER_STATUS_ENABLED).asString();
    }

    /**
     * @brief 从数据库行加载
     */
    Task<void> load(int userId) {
        std::string sql = R"(
            SELECT u.*, d.name as department_name
            FROM sys_user u
            LEFT JOIN sys_department d ON u.department_id = d.id AND d.deleted_at IS NULL
            WHERE u.id = ? AND u.deleted_at IS NULL
        )";
        auto result = co_await db().execSqlCoro(sql, {std::to_string(userId)});

        if (result.empty()) {
            throw NotFoundException("用户不存在");
        }

        fromRow(result[0]);
        co_await loadRoles();
        markLoaded();
    }

    /**
     * @brief 从数据库行填充字段
     */
    void fromRow(const Row& row) {
        setId(FieldHelper::getInt(row["id"]));
        username_ = FieldHelper::getString(row["username"]);
        passwordHash_ = FieldHelper::getString(row["password_hash"], "");
        nickname_ = FieldHelper::getString(row["nickname"], "");
        phone_ = FieldHelper::getString(row["phone"], "");
        email_ = FieldHelper::getString(row["email"], "");
        departmentId_ = FieldHelper::getInt(row["department_id"], 0);
        status_ = FieldHelper::getString(row["status"], Constants::USER_STATUS_ENABLED);
        createdAt_ = FieldHelper::getString(row["created_at"], "");
        updatedAt_ = FieldHelper::getString(row["updated_at"], "");

        try {
            departmentName_ = FieldHelper::getString(row["department_name"], "");
        } catch (...) {
            departmentName_ = "";
        }
    }

    /**
     * @brief 加载用户角色
     */
    Task<void> loadRoles() {
        auto result = co_await db().execSqlCoro(R"(
            SELECT r.id, r.name, r.code
            FROM sys_role r
            INNER JOIN sys_user_role ur ON r.id = ur.role_id
            WHERE ur.user_id = ? AND r.deleted_at IS NULL
        )", {std::to_string(id())});

        roles_ = Json::Value(Json::arrayValue);
        roleIds_.clear();

        for (const auto& row : result) {
            Json::Value role;
            role["id"] = FieldHelper::getInt(row["id"]);
            role["name"] = FieldHelper::getString(row["name"]);
            role["code"] = FieldHelper::getString(row["code"]);
            roles_.append(role);
            roleIds_.push_back(FieldHelper::getInt(row["id"]));
        }
    }

    /**
     * @brief 批量加载用户角色
     */
    static Task<std::map<int, Json::Value>> batchLoadRoles(const std::vector<int>& userIds) {
        std::map<int, Json::Value> result;
        if (userIds.empty()) co_return result;

        for (int uid : userIds) {
            result[uid] = Json::Value(Json::arrayValue);
        }

        std::string idList = SqlHelper::buildInClause(userIds);

        DatabaseService db;
        std::string sql =
            "SELECT ur.user_id, r.id, r.name, r.code "
            "FROM sys_role r "
            "INNER JOIN sys_user_role ur ON r.id = ur.role_id "
            "WHERE ur.user_id IN (" + idList + ") "
            "AND r.deleted_at IS NULL "
            "ORDER BY ur.user_id, r.id";
        auto rows = co_await db.execSqlCoro(sql);

        for (const auto& row : rows) {
            int userId = FieldHelper::getInt(row["user_id"]);
            Json::Value role;
            role["id"] = FieldHelper::getInt(row["id"]);
            role["name"] = FieldHelper::getString(row["name"]);
            role["code"] = FieldHelper::getString(row["code"]);
            result[userId].append(role);
        }

        co_return result;
    }

    // ==================== 持久化操作 ====================

    Task<void> persistCreate(TransactionGuard& tx) {
        auto result = co_await tx.execSqlCoro(R"(
            INSERT INTO sys_user (username, password_hash, nickname, phone, email,
                                  department_id, status, created_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?) RETURNING id
        )", {
            username_, passwordHash_, nickname_, phone_, email_,
            std::to_string(departmentId_), status_, TimestampHelper::now()
        });

        setId(FieldHelper::getInt(result[0]["id"]));
        raiseEvent<UserCreated>(id(), username_);
    }

    Task<void> persistUpdate(TransactionGuard& tx) {
        co_await tx.execSqlCoro(R"(
            UPDATE sys_user
            SET nickname = ?, phone = ?, email = ?, department_id = ?,
                status = ?, password_hash = ?, updated_at = ?
            WHERE id = ?
        )", {
            nickname_, phone_, email_, std::to_string(departmentId_),
            status_, passwordHash_, TimestampHelper::now(), std::to_string(id())
        });

        raiseEvent<UserUpdated>(id());
    }

    Task<void> persistDelete(TransactionGuard& tx) {
        // 删除角色关联
        co_await tx.execSqlCoro(
            "DELETE FROM sys_user_role WHERE user_id = ?",
            {std::to_string(id())}
        );

        // 软删除用户
        co_await tx.execSqlCoro(
            "UPDATE sys_user SET deleted_at = ? WHERE id = ?",
            {TimestampHelper::now(), std::to_string(id())}
        );

        raiseEvent<UserDeleted>(id());
    }

    Task<void> persistRoles(TransactionGuard& tx) {
        // 删除现有角色
        co_await tx.execSqlCoro(
            "DELETE FROM sys_user_role WHERE user_id = ?",
            {std::to_string(id())}
        );

        // 插入新角色
        if (!pendingRoleIds_.empty()) {
            auto [valuesSql, params] = SqlHelper::buildBatchInsertValues(id(), pendingRoleIds_);
            co_await tx.execSqlCoro(
                "INSERT INTO sys_user_role (user_id, role_id) VALUES " + valuesSql, params);
        }

        raiseEvent<UserRolesChanged>(id(), pendingRoleIds_);
        rolesChanged_ = false;
    }
};
