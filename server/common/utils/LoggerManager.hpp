#pragma once

#include <trantor/utils/Logger.h>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

/**
 * @brief 日志管理器 - 管理文件日志和控制台输出
 */
class LoggerManager {
private:
    static std::ofstream logFile_;
    static std::mutex logMutex_;
    static bool consoleEnabled_;

    /**
     * @brief 格式化日志消息
     * @details 将 Drogon 原始日志格式转换为更易读的格式
     *          原始: "YYYYMMDD HH:MM:SS.microseconds ThreadID Level [func] message - file:line"
     *          目标: "YYYY-MM-DD HH:MM:SS ThreadID Level message"
     */
    static std::string formatLogMessage(const char* msg, uint64_t len) {
        std::string logMsg(msg, len);

        if (len >= 17 && logMsg[8] == ' ') {
            size_t timeEnd = logMsg.find(' ', 9);
            if (timeEnd != std::string::npos && timeEnd > 15) {
                std::string year = logMsg.substr(0, 4);
                std::string month = logMsg.substr(4, 2);
                std::string day = logMsg.substr(6, 2);
                std::string time = logMsg.substr(9, 8);

                std::string rest = logMsg.substr(timeEnd);

                // 移除 [operator ()] - lambda 函数名无意义
                size_t opStart = rest.find("[operator ()");
                if (opStart != std::string::npos) {
                    size_t opEnd = rest.find("] ", opStart);
                    if (opEnd != std::string::npos) {
                        rest = rest.substr(0, opStart) + rest.substr(opEnd + 2);
                    }
                }

                // 移除末尾的 " - file.cpp:line"
                size_t filePos = rest.rfind(" - ");
                if (filePos != std::string::npos) {
                    std::string suffix = rest.substr(filePos + 3);
                    if (suffix.find(".cpp:") != std::string::npos ||
                        suffix.find(".hpp:") != std::string::npos) {
                        rest = rest.substr(0, filePos) + "\n";
                    }
                }

                return year + "-" + month + "-" + day + " " + time + rest;
            }
        }
        return logMsg;
    }

    /**
     * @brief 日志输出函数
     */
    static void outputFunction(const char* msg, const uint64_t len) {
        std::string formatted = formatLogMessage(msg, len);
        std::lock_guard<std::mutex> lock(logMutex_);
        if (logFile_.is_open()) {
            logFile_ << formatted;
        }
        if (consoleEnabled_) {
            std::cout << formatted;
        }
    }

    /**
     * @brief 日志刷新函数
     */
    static void flushFunction() {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (logFile_.is_open()) {
            logFile_.flush();
        }
        if (consoleEnabled_) {
            std::cout.flush();
        }
    }

public:
    /**
     * @brief 初始化日志系统
     * @param logFilePath 日志文件路径
     * @param consoleOutput 是否输出到控制台
     */
    static void initialize(const std::string& logFilePath, bool consoleOutput = false) {
        // 创建日志目录
        fs::path logPath(logFilePath);
        if (logPath.has_parent_path()) {
            fs::create_directories(logPath.parent_path());
        }

        // 打开日志文件
        logFile_.open(logFilePath, std::ios::app);
        consoleEnabled_ = consoleOutput;

        // 设置显示本地时间
        trantor::Logger::setDisplayLocalTime(true);

        // 注册日志输出函数
        trantor::Logger::setOutputFunction(outputFunction, flushFunction);

        // 设置默认日志级别
        trantor::Logger::setLogLevel(trantor::Logger::kInfo);
    }

    /**
     * @brief 设置日志级别
     */
    static void setLogLevel(const std::string& level) {
        if (level == "TRACE") {
            trantor::Logger::setLogLevel(trantor::Logger::kTrace);
        } else if (level == "DEBUG") {
            trantor::Logger::setLogLevel(trantor::Logger::kDebug);
        } else if (level == "INFO") {
            trantor::Logger::setLogLevel(trantor::Logger::kInfo);
        } else if (level == "WARN") {
            trantor::Logger::setLogLevel(trantor::Logger::kWarn);
        } else if (level == "ERROR") {
            trantor::Logger::setLogLevel(trantor::Logger::kError);
        } else if (level == "FATAL") {
            trantor::Logger::setLogLevel(trantor::Logger::kFatal);
        }
    }

    /**
     * @brief 启用/禁用控制台输出
     */
    static void setConsoleOutput(bool enabled) {
        consoleEnabled_ = enabled;
    }

    /**
     * @brief 关闭日志文件
     */
    static void close() {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (logFile_.is_open()) {
            logFile_.close();
        }
    }
};

// 静态成员初始化
std::ofstream LoggerManager::logFile_;
std::mutex LoggerManager::logMutex_;
bool LoggerManager::consoleEnabled_ = false;
