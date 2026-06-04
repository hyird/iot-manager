#pragma once

#include <cstddef>
#include <deque>
#include <utility>

enum class ProtocolJobPriority {
    High,
    Normal
};

/**
 * @brief 协议 session 内优先级任务队列
 *
 * 统一“高优先级控制 / 普通轮询或发现”的排队语义。队列只负责优先级、容量和过滤，
 * 任务如何发送、如何匹配应答、如何超时仍由协议引擎决定。
 */
template<typename Job>
class ProtocolJobQueue {
public:
    explicit ProtocolJobQueue(std::size_t maxSize = 256)
        : maxSize_(maxSize) {}

    bool push(Job job, ProtocolJobPriority priority = ProtocolJobPriority::Normal) {
        if (isFull()) {
            return false;
        }
        queue(priority).push_back(std::move(job));
        return true;
    }

    bool empty() const {
        return high_.empty() && normal_.empty();
    }

    bool isFull() const {
        return size() >= maxSize_;
    }

    std::size_t size() const {
        return high_.size() + normal_.size();
    }

    std::size_t available() const {
        const auto currentSize = size();
        return currentSize >= maxSize_ ? 0 : maxSize_ - currentSize;
    }

    bool canPush(std::size_t count = 1) const {
        return count <= available();
    }

    std::size_t highSize() const {
        return high_.size();
    }

    std::size_t normalSize() const {
        return normal_.size();
    }

    Job* peek() {
        if (!high_.empty()) {
            return &high_.front();
        }
        if (!normal_.empty()) {
            return &normal_.front();
        }
        return nullptr;
    }

    const Job* peek() const {
        if (!high_.empty()) {
            return &high_.front();
        }
        if (!normal_.empty()) {
            return &normal_.front();
        }
        return nullptr;
    }

    bool popNext(Job& out) {
        auto* source = !high_.empty() ? &high_ : &normal_;
        if (source->empty()) {
            return false;
        }
        out = std::move(source->front());
        source->pop_front();
        return true;
    }

    void dropNext() {
        if (!high_.empty()) {
            high_.pop_front();
            return;
        }
        if (!normal_.empty()) {
            normal_.pop_front();
        }
    }

    void clear() {
        high_.clear();
        normal_.clear();
    }

    template<typename Predicate>
    std::size_t removeIf(Predicate predicate) {
        return filterQueue(high_, predicate) + filterQueue(normal_, predicate);
    }

private:
    std::deque<Job>& queue(ProtocolJobPriority priority) {
        return priority == ProtocolJobPriority::High ? high_ : normal_;
    }

    template<typename Predicate>
    static std::size_t filterQueue(std::deque<Job>& source, Predicate predicate) {
        std::deque<Job> kept;
        std::size_t removed = 0;
        for (auto& job : source) {
            if (!predicate(job)) {
                kept.push_back(std::move(job));
            } else {
                ++removed;
            }
        }
        source = std::move(kept);
        return removed;
    }

    std::size_t maxSize_;
    std::deque<Job> high_;
    std::deque<Job> normal_;
};
