#pragma once

namespace fs = std::filesystem;

/**
 * @brief 日志管理器 - 使用 trantor::AsyncFileLogger 异步写盘 + 按日期轮转
 *
 * 文件命名: logs/iot-manager_YYYY-MM-DD_HHMMSS.log
 * 轮转策略: 每天自动创建新文件 + 单文件超 100MB 时轮转
 */
class LoggerManager {
private:
    static std::unique_ptr<trantor::AsyncFileLogger> fileLogger_;
    static std::shared_mutex loggerMutex_;
    static std::string logDir_;
    static std::atomic<int> currentDay_;
    static constexpr uint64_t FILE_SIZE_LIMIT = 100 * 1024 * 1024;  // 100MB

    /** 获取当天日期整数 YYYYMMDD，用于快速比较 */
    static int todayInt() {
        auto now = std::chrono::system_clock::now();
        auto dp = std::chrono::floor<std::chrono::days>(now);
        std::chrono::year_month_day ymd{dp};
        return static_cast<int>(ymd.year()) * 10000
             + static_cast<unsigned>(ymd.month()) * 100
             + static_cast<unsigned>(ymd.day());
    }

    /** YYYYMMDD 整数转 "YYYY-MM-DD" 字符串 */
    static std::string dayToStr(int day) {
        char buf[11];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      day / 10000, day % 10000 / 100, day % 100);
        return buf;
    }

    /** 创建并启动 AsyncFileLogger */
    static std::unique_ptr<trantor::AsyncFileLogger> createLogger(int day) {
        auto logger = std::make_unique<trantor::AsyncFileLogger>();
        logger->setFileName(logDir_ + "/iot-manager_" + dayToStr(day));
        logger->setFileSizeLimit(FILE_SIZE_LIMIT);
        logger->startLogging();
        return logger;
    }

    /** 日期变更时轮转日志文件 */
    static void rotateDailyLog(int today) {
        std::unique_ptr<trantor::AsyncFileLogger> oldLogger;
        {
            std::unique_lock lock(loggerMutex_);
            if (today == currentDay_.load(std::memory_order_relaxed)) return;

            oldLogger = std::move(fileLogger_);
            fileLogger_ = createLogger(today);
            currentDay_.store(today, std::memory_order_relaxed);
        }
        // oldLogger 在锁外析构，自动 flush 剩余数据
    }

    /**
     * @brief 格式化日志消息
     * @details 将 trantor 原始日志格式转换为更易读的格式
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

    /** 日志输出函数（注册到 trantor::Logger） */
    static void outputFunction(const char* msg, const uint64_t len) {
        std::string formatted = formatLogMessage(msg, len);

        // 检查日期轮转（原子整数比较，开销极小）
        int today = todayInt();
        if (today != currentDay_.load(std::memory_order_relaxed)) {
            rotateDailyLog(today);
        }

        std::shared_lock lock(loggerMutex_);
        if (fileLogger_) {
            fileLogger_->output(formatted.c_str(), formatted.size());
        }
    }

    /** 日志刷新函数（注册到 trantor::Logger） */
    static void flushFunction() {
        std::shared_lock lock(loggerMutex_);
        if (fileLogger_) {
            fileLogger_->flush();
        }
    }

public:
    /**
     * @brief 初始化日志系统
     * @param logDir 日志目录路径
     */
    static void initialize(const std::string& logDir) {
        fs::create_directories(logDir);
        logDir_ = logDir;

        int today = todayInt();
        currentDay_.store(today, std::memory_order_relaxed);
        fileLogger_ = createLogger(today);

        trantor::Logger::setDisplayLocalTime(true);
        trantor::Logger::setOutputFunction(outputFunction, flushFunction);
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
     * @brief 关闭日志系统
     */
    static void close() {
        std::unique_lock lock(loggerMutex_);
        fileLogger_.reset();
    }
};

// 静态成员初始化（inline 避免多翻译单元 ODR 违规）
inline std::unique_ptr<trantor::AsyncFileLogger> LoggerManager::fileLogger_;
inline std::shared_mutex LoggerManager::loggerMutex_;
inline std::string LoggerManager::logDir_;
inline std::atomic<int> LoggerManager::currentDay_{0};
