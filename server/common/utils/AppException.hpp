#pragma once

/**
 * @brief 应用异常基类
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
 * 错误码定义：
 * 1xxx - 通用错误
 * 2xxx - 认证相关错误
 */

/**
 * @brief 通用异常 - 资源不存在
 */
class NotFoundException : public AppException {
public:
    explicit NotFoundException(const std::string& message = "资源不存在")
        : AppException(1001, message, k404NotFound) {}
};

/**
 * @brief 通用异常 - 验证失败
 */
class ValidationException : public AppException {
public:
    explicit ValidationException(const std::string& message = "验证失败")
        : AppException(1002, message, k400BadRequest) {}
};

/**
 * @brief 通用异常 - 禁止访问
 */
class ForbiddenException : public AppException {
public:
    explicit ForbiddenException(const std::string& message = "禁止访问")
        : AppException(1003, message, k403Forbidden) {}
};

/**
 * @brief 认证相关异常
 */
namespace AuthException {
    using enum drogon::HttpStatusCode;

    inline AppException UserNotFound() {
        return AppException(2001, "用户不存在", k404NotFound);
    }

    inline AppException PasswordIncorrect() {
        return AppException(2002, "用户名或密码错误", k403Forbidden);
    }

    inline AppException UserDisabled() {
        return AppException(2003, "用户已被禁用", k403Forbidden);
    }

    inline AppException TokenInvalid() {
        return AppException(2004, "令牌无效或已过期", k401Unauthorized);
    }

    inline AppException TokenExpired() {
        return AppException(2005, "令牌已过期", k401Unauthorized);
    }

    inline AppException NoPermission() {
        return AppException(2006, "无权限访问", k403Forbidden);
    }
}
