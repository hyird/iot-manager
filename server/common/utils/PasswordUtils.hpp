#pragma once

/**
 * @brief 密码工具类（使用 PBKDF2 哈希）
 */
class PasswordUtils {
private:
    static constexpr int SALT_LENGTH = 16;
    static constexpr int HASH_ITERATIONS = 10000;
    static constexpr int HASH_LENGTH = 32;

    static std::string generateSalt() {
        unsigned char salt[SALT_LENGTH];
        RAND_bytes(salt, SALT_LENGTH);

        std::ostringstream oss;
        for (int i = 0; i < SALT_LENGTH; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(salt[i]);
        }
        return oss.str();
    }

    static std::vector<unsigned char> hexToBytes(const std::string& hex) {
        std::vector<unsigned char> bytes;
        for (size_t i = 0; i + 1 < hex.length(); i += 2) {
            uint8_t value{};
            std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
            bytes.push_back(value);
        }
        return bytes;
    }

    static std::string pbkdf2Hash(const std::string& password, const std::string& saltHex) {
        auto saltBytes = hexToBytes(saltHex);
        unsigned char hash[HASH_LENGTH];

        PKCS5_PBKDF2_HMAC(password.c_str(),
                          static_cast<int>(password.length()),
                          saltBytes.data(),
                          static_cast<int>(saltBytes.size()),
                          HASH_ITERATIONS,
                          EVP_sha256(),
                          HASH_LENGTH,
                          hash);

        std::ostringstream oss;
        for (int i = 0; i < HASH_LENGTH; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(hash[i]);
        }
        return oss.str();
    }

public:
    static std::string hashPassword(const std::string& password) {
        std::string salt = generateSalt();
        std::string hash = pbkdf2Hash(password, salt);
        return salt + "$" + hash;
    }

    static bool verifyPassword(const std::string& password, const std::string& hashedPassword) {
        size_t dollarPos = hashedPassword.find('$');
        if (dollarPos == std::string::npos) {
            return false;
        }

        std::string salt = hashedPassword.substr(0, dollarPos);
        std::string expectedHash = hashedPassword.substr(dollarPos + 1);
        std::string actualHash = pbkdf2Hash(password, salt);

        // 常量时间比较，防止时序攻击
        return actualHash.size() == expectedHash.size() &&
               CRYPTO_memcmp(actualHash.data(), expectedHash.data(), actualHash.size()) == 0;
    }

    static std::string generateRandomPassword(int length = 12) {
        const std::string chars =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "!@#$%^&*";

        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> distribution(0, static_cast<int>(chars.size()) - 1);

        std::string password;
        password.reserve(length);
        for (int i = 0; i < length; ++i) {
            password += chars[distribution(generator)];
        }

        return password;
    }
};
