#pragma once

#include <algorithm>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <trantor/net/EventLoop.h>

namespace coroutine_executor_detail {

template <typename Result>
struct AwaiterStorage {
    using Type = std::optional<Result>;
};

template <>
struct AwaiterStorage<void> {
    using Type = std::monostate;
};

} // namespace coroutine_executor_detail

/**
 * @brief Tiny coroutine-aware worker pool for blocking driver calls.
 *
 * The awaiter runs the submitted callable on a worker thread, then resumes the
 * awaiting coroutine on the EventLoop that suspended it.
 */
class CoroutineExecutor {
    template <typename Result, typename Fn>
    class Awaiter;

public:
    static CoroutineExecutor& instance() {
        static CoroutineExecutor executor;
        return executor;
    }

    template <typename Fn>
    auto submit(Fn&& fn) {
        using FnType = std::decay_t<Fn>;
        using Result = std::invoke_result_t<FnType&>;
        return Awaiter<Result, FnType>(*this, std::forward<Fn>(fn), 0, false);
    }

    template <typename Fn>
    auto submitSerial(const void* key, bool highPriority, Fn&& fn) {
        return submitSerialKey(reinterpret_cast<std::uintptr_t>(key), highPriority, std::forward<Fn>(fn));
    }

    template <typename Fn>
    auto submitSerialKey(std::uintptr_t key, bool highPriority, Fn&& fn) {
        using FnType = std::decay_t<Fn>;
        using Result = std::invoke_result_t<FnType&>;
        return Awaiter<Result, FnType>(
            *this,
            std::forward<Fn>(fn),
            key,
            highPriority);
    }

private:
    using Job = std::function<void()>;

    struct SerialQueue {
        std::queue<Job> highPriorityJobs;
        std::queue<Job> jobs;
        bool running = false;
    };

    CoroutineExecutor() {
        const unsigned int hardware = std::thread::hardware_concurrency();
        const unsigned int workerCount = std::clamp(hardware == 0 ? 2U : hardware / 2U, 2U, 4U);
        workers_.reserve(workerCount);
        for (unsigned int i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~CoroutineExecutor() {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    CoroutineExecutor(const CoroutineExecutor&) = delete;
    CoroutineExecutor& operator=(const CoroutineExecutor&) = delete;

    void enqueue(Job job) {
        {
            std::lock_guard lock(mutex_);
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

    void enqueueSerial(std::uintptr_t key, bool highPriority, Job job) {
        if (key == 0) {
            enqueue(std::move(job));
            return;
        }

        {
            std::lock_guard lock(mutex_);
            auto& queue = serialQueues_[key];
            if (highPriority) {
                queue.highPriorityJobs.push(std::move(job));
            } else {
                queue.jobs.push(std::move(job));
            }
            if (!queue.running) {
                queue.running = true;
                enqueueNextSerialJobLocked(key, queue);
            }
        }
        cv_.notify_one();
    }

    void enqueueNextSerialJobLocked(std::uintptr_t key, SerialQueue& queue) {
        Job next;
        bool highPriority = false;
        if (!queue.highPriorityJobs.empty()) {
            next = std::move(queue.highPriorityJobs.front());
            queue.highPriorityJobs.pop();
            highPriority = true;
        } else if (!queue.jobs.empty()) {
            next = std::move(queue.jobs.front());
            queue.jobs.pop();
        } else {
            queue.running = false;
            serialQueues_.erase(key);
            return;
        }

        auto wrapped = [this, key, job = std::move(next)]() mutable {
            job();
            finishSerialJob(key);
        };
        if (highPriority) {
            highPriorityJobs_.push(std::move(wrapped));
        } else {
            jobs_.push(std::move(wrapped));
        }
    }

    void finishSerialJob(std::uintptr_t key) {
        {
            std::lock_guard lock(mutex_);
            auto it = serialQueues_.find(key);
            if (it != serialQueues_.end()) {
                enqueueNextSerialJobLocked(key, it->second);
            }
        }
        cv_.notify_one();
    }

    void workerLoop() {
        while (true) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this]() {
                    return stopping_ || !highPriorityJobs_.empty() || !jobs_.empty();
                });
                if (stopping_ && highPriorityJobs_.empty() && jobs_.empty()) {
                    return;
                }
                if (!highPriorityJobs_.empty()) {
                    job = std::move(highPriorityJobs_.front());
                    highPriorityJobs_.pop();
                } else {
                    job = std::move(jobs_.front());
                    jobs_.pop();
                }
            }
            job();
        }
    }

    template <typename Result, typename Fn>
    class Awaiter {
    public:
        Awaiter(CoroutineExecutor& executor, Fn&& fn, std::uintptr_t serialKey, bool highPriority)
            : executor_(executor)
            , fn_(std::move(fn))
            , serialKey_(serialKey)
            , highPriority_(highPriority) {}

        bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            resumeLoop_ = trantor::EventLoop::getEventLoopOfCurrentThread();
            executor_.enqueueSerial(serialKey_, highPriority_, [this, handle]() mutable {
                try {
                    if constexpr (std::is_void_v<Result>) {
                        std::invoke(fn_);
                    } else {
                        result_.emplace(std::invoke(fn_));
                    }
                } catch (...) {
                    exception_ = std::current_exception();
                }

                if (resumeLoop_ != nullptr) {
                    resumeLoop_->queueInLoop([handle]() mutable {
                        handle.resume();
                    });
                } else {
                    handle.resume();
                }
            });
        }

        decltype(auto) await_resume() {
            if (exception_) {
                std::rethrow_exception(exception_);
            }

            if constexpr (std::is_void_v<Result>) {
                return;
            } else {
                return std::move(*result_);
            }
        }

    private:
        CoroutineExecutor& executor_;
        Fn fn_;
        std::uintptr_t serialKey_{0};
        bool highPriority_{false};
        trantor::EventLoop* resumeLoop_{nullptr};
        std::exception_ptr exception_;
        [[no_unique_address]] typename coroutine_executor_detail::AwaiterStorage<Result>::Type result_;
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Job> highPriorityJobs_;
    std::queue<Job> jobs_;
    std::unordered_map<std::uintptr_t, SerialQueue> serialQueues_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};
