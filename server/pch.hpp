// 预编译头文件 (PCH)
// 仅包含稳定的标准库和第三方库头文件
// 不包含项目内部头文件（变化频繁会导致 PCH 频繁重建）
#pragma once

// Windows: 禁用 min/max 宏，避免与 std::min/std::max 冲突
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

// ==================== C++ 标准库 ====================

// 容器
#include <string>
#include <vector>
#include <map>
#include <set>
#include <array>
#include <unordered_map>

// 工具
#include <functional>
#include <optional>
#include <memory>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <tuple>
#include <typeindex>

// IO / 格式化
#include <sstream>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <filesystem>

// 其他
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <numeric>
#include <bit>
#include <utility>
#include <deque>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <random>
#include <regex>
#include <future>

// ==================== Drogon / Trantor 框架 ====================

#include <drogon/drogon.h>
#include <drogon/HttpController.h>
#include <drogon/HttpFilter.h>
#include <drogon/WebSocketController.h>
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Field.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/utils/Utilities.h>

#include <trantor/net/TcpServer.h>
#include <trantor/net/TcpClient.h>
#include <trantor/net/EventLoopThreadPool.h>
#include <trantor/utils/Logger.h>

// ==================== 第三方库 ====================

#include <json/json.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
