#pragma once

#include <drogon/HttpTypes.h>

#include "ErrorCodes.hpp"

/**
 * @brief 应用异常基类
 *
 * AppException 只负责承载错误码、消息和 HTTP 状态；
 * 具体错误码定义统一放在 ErrorCodes.hpp 中。
 */
class AppException : public std::exception {
public:
    using HttpStatusCode = drogon::HttpStatusCode;
    using enum drogon::HttpStatusCode;

private:
    int code_;
    std::string message_;
    HttpStatusCode status_;

public:
    AppException(int code, std::string message, HttpStatusCode status = k400BadRequest)
        : code_(code), message_(std::move(message)), status_(status) {}

    const char* what() const noexcept override {
        return message_.c_str();
    }

    int getCode() const { return code_; }
    const std::string& getMessage() const { return message_; }
    HttpStatusCode getStatus() const { return status_; }
};

/**
 * @brief 通用异常 - 资源不存在
 */
class NotFoundException : public AppException {
public:
    explicit NotFoundException(const std::string& message = "资源不存在")
        : AppException(ErrorCodes::NOT_FOUND, message, k404NotFound) {}
};

/**
 * @brief 通用异常 - 验证失败
 */
class ValidationException : public AppException {
public:
    explicit ValidationException(const std::string& message = "验证失败")
        : AppException(ErrorCodes::VALIDATION_FAILED, message, k400BadRequest) {}
};

/**
 * @brief 通用异常 - 数据冲突
 */
class ConflictException : public AppException {
public:
    explicit ConflictException(const std::string& message = "数据冲突")
        : AppException(ErrorCodes::CONFLICT, message, k409Conflict) {}
};

/**
 * @brief 通用异常 - 禁止访问
 */
class ForbiddenException : public AppException {
public:
    explicit ForbiddenException(const std::string& message = "禁止访问")
        : AppException(ErrorCodes::FORBIDDEN, message, k403Forbidden) {}
};

/**
 * @brief 认证相关异常
 */
namespace AuthException {
    using enum drogon::HttpStatusCode;

    inline AppException UserNotFound(std::string message = "用户不存在") {
        return AppException(ErrorCodes::USER_NOT_FOUND, std::move(message), k404NotFound);
    }

    inline AppException PasswordIncorrect(std::string message = "用户名或密码错误") {
        return AppException(ErrorCodes::PASSWORD_INCORRECT, std::move(message), k403Forbidden);
    }

    inline AppException UserDisabled(std::string message = "用户已被禁用") {
        return AppException(ErrorCodes::USER_DISABLED, std::move(message), k403Forbidden);
    }

    inline AppException TokenInvalid(std::string message = "令牌无效或已过期") {
        return AppException(ErrorCodes::UNAUTHORIZED, std::move(message), k401Unauthorized);
    }

    inline AppException TokenExpired(std::string message = "令牌已过期") {
        return AppException(ErrorCodes::TOKEN_EXPIRED, std::move(message), k401Unauthorized);
    }

    inline AppException TokenBlacklisted(std::string message = "令牌已失效，请重新登录") {
        return AppException(ErrorCodes::TOKEN_BLACKLISTED, std::move(message), k401Unauthorized);
    }

    inline AppException NoPermission(std::string message = "无权限访问") {
        return AppException(ErrorCodes::NO_PERMISSION, std::move(message), k403Forbidden);
    }
}
