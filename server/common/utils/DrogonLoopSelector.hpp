#pragma once

#include <drogon/drogon.h>
#include <atomic>

/**
 * @brief Drogon IO 线程选择器
 *
 * 统一管理 IO 线程分配策略，避免各组件硬编码 getIOLoop(0) 导致单线程过载。
 *
 * 使用场景：
 * - getNext(): 无串行化需求的独立任务（Redis 写入、协议重加载等）
 * - fixed(0):  需要单线程串行化的操作（攒批写入）
 */
class DrogonLoopSelector {
public:
    /**
     * @brief Round-robin 轮转分配 IO 线程
     * 每次调用返回下一个 IO 线程，均匀分散负载。
     */
    static trantor::EventLoop* getNext() {
        size_t n = drogon::app().getThreadNum();
        if (n == 0) return drogon::app().getLoop();
        size_t idx = index_.fetch_add(1, std::memory_order_relaxed) % n;
        return drogon::app().getIOLoop(idx);
    }

    /**
     * @brief 获取固定索引的 IO 线程
     * 用于需要串行化访问的场景（如攒批写入）。
     */
    static trantor::EventLoop* fixed(size_t idx) {
        size_t n = drogon::app().getThreadNum();
        if (n == 0) return drogon::app().getLoop();
        return drogon::app().getIOLoop(idx % n);
    }

private:
    static inline std::atomic<size_t> index_{0};
};
