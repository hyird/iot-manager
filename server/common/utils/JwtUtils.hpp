#pragma once

#include "AppException.hpp"

/**
 * @brief JWT 工具类
 * 实现 HS256 签名算法
 */
class JwtUtils {
private:
    std::string secret_;
    int expiresIn_;

    // RAII wrapper for BIO chain
    using BioChainPtr = std::unique_ptr<BIO, decltype(&BIO_free_all)>;

    static std::string base64UrlEncode(const std::string& input) {
        BioChainPtr bioChain(
            BIO_push(BIO_new(BIO_f_base64()), BIO_new(BIO_s_mem())),
            BIO_free_all
        );

        BIO_set_flags(bioChain.get(), BIO_FLAGS_BASE64_NO_NL);
        BIO_write(bioChain.get(), input.c_str(), static_cast<int>(input.length()));
        BIO_flush(bioChain.get());

        BUF_MEM* bufferPtr = nullptr;  // Points to internal BIO memory, not owned
        BIO_get_mem_ptr(bioChain.get(), &bufferPtr);

        std::string result(bufferPtr->data, bufferPtr->length);

        for (char& c : result) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        result.erase(std::remove(result.begin(), result.end(), '='), result.end());

        return result;
    }

    static std::string base64UrlDecode(std::string input) {
        for (char& c : input) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }

        while (input.length() % 4) {
            input += '=';
        }

        std::vector<char> buffer(input.length(), 0);

        BioChainPtr bioChain(
            BIO_push(BIO_new(BIO_f_base64()), BIO_new_mem_buf(input.c_str(), static_cast<int>(input.length()))),
            BIO_free_all
        );

        BIO_set_flags(bioChain.get(), BIO_FLAGS_BASE64_NO_NL);
        int decodedLength = BIO_read(bioChain.get(), buffer.data(), static_cast<int>(input.length()));

        return std::string(buffer.data(), decodedLength);
    }

    std::string hmacSha256(const std::string& data) const {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int digestLen = 0;

        HMAC(EVP_sha256(),
             secret_.c_str(),
             static_cast<int>(secret_.length()),
             reinterpret_cast<const unsigned char*>(data.c_str()),
             data.length(),
             digest.data(),
             &digestLen);

        return std::string(reinterpret_cast<char*>(digest.data()), digestLen);
    }

public:
    JwtUtils(std::string secret, int expiresIn = 3600)
        : secret_(std::move(secret)), expiresIn_(expiresIn) {}

    std::string sign(const Json::Value& payload) const {
        Json::Value header;
        header["alg"] = "HS256";
        header["typ"] = "JWT";

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::string headerStr = Json::writeString(builder, header);
        std::string encodedHeader = base64UrlEncode(headerStr);

        Json::Value finalPayload = payload;
        auto now = std::chrono::system_clock::now();
        auto exp = now + std::chrono::seconds(expiresIn_);
        auto iat = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        auto expTime = std::chrono::duration_cast<std::chrono::seconds>(
            exp.time_since_epoch()).count();

        finalPayload["iat"] = Json::Value::Int64(iat);
        finalPayload["exp"] = Json::Value::Int64(expTime);

        std::string payloadStr = Json::writeString(builder, finalPayload);
        std::string encodedPayload = base64UrlEncode(payloadStr);

        std::string message = encodedHeader + "." + encodedPayload;
        std::string signature = hmacSha256(message);
        std::string encodedSignature = base64UrlEncode(signature);

        return encodedHeader + "." + encodedPayload + "." + encodedSignature;
    }

    Json::Value verify(const std::string& token) const {
        size_t firstDot = token.find('.');
        size_t secondDot = token.find('.', firstDot + 1);

        if (firstDot == std::string::npos || secondDot == std::string::npos) {
            throw AuthException::TokenInvalid();
        }

        std::string encodedHeader = token.substr(0, firstDot);
        std::string encodedPayload = token.substr(firstDot + 1, secondDot - firstDot - 1);
        std::string encodedSignature = token.substr(secondDot + 1);

        std::string message = encodedHeader + "." + encodedPayload;
        std::string expectedSignature = hmacSha256(message);
        std::string expectedEncodedSignature = base64UrlEncode(expectedSignature);

        // 常量时间比较，防止时序攻击
        if (encodedSignature.size() != expectedEncodedSignature.size() ||
            CRYPTO_memcmp(encodedSignature.data(), expectedEncodedSignature.data(),
                          expectedEncodedSignature.size()) != 0) {
            throw AuthException::TokenInvalid();
        }

        std::string payloadStr = base64UrlDecode(encodedPayload);
        Json::CharReaderBuilder readerBuilder;
        Json::Value payload;
        std::istringstream iss(payloadStr);
        std::string errs;

        if (!Json::parseFromStream(readerBuilder, iss, &payload, &errs)) {
            throw AuthException::TokenInvalid();
        }

        if (payload.isMember("exp")) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto exp = payload["exp"].asInt64();

            if (now > exp) {
                throw AuthException::TokenExpired();
            }
        }

        return payload;
    }
};
