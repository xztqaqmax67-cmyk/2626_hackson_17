#pragma once
#include "common.hpp"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace problem2 {
namespace parallel_detail {
inline thread_local bool inside_batch = false;

// 同一个进程只增加三条计算线程。调用线程也参与；嵌套任务就地执行，
// 避免候选评分再按核展开时耗尽线程池或超过线程数限制。
class Pool {
    std::mutex batch_mutex_, mutex_;
    std::condition_variable changed_, finished_;
    std::vector<std::thread> workers_;
    std::function<void(std::size_t)> work_;
    std::atomic<std::size_t> next_{0};
    std::size_t count_ = 0, generation_ = 0, pending_ = 0;
    double deadline_ = 0;
    bool stop_ = false;

    void consume() noexcept {
        const auto old_deadline = execution_deadline;
        const auto old_batch = inside_batch;
        execution_deadline = deadline_;
        inside_batch = true;
        for (;;) {
            const auto index = next_.fetch_add(1, std::memory_order_relaxed);
            if (index >= count_) break;
            work_(index); // 包装层收集每个槽位的异常，不跨线程抛出。
        }
        inside_batch = old_batch;
        execution_deadline = old_deadline;
    }
    void worker() {
        std::size_t observed = 0;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            changed_.wait(lock, [&] { return stop_ || generation_ != observed; });
            if (stop_) return;
            observed = generation_;
            lock.unlock();
            consume();
            lock.lock();
            if (--pending_ == 0) finished_.notify_one();
        }
    }
    void close() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        for (auto &thread : workers_)
            if (thread.joinable()) thread.join();
    }

public:
    Pool() {
        workers_.reserve(3);
        try {
            for (int i = 0; i < 3; ++i) {
                try {
                    workers_.emplace_back([this] { worker(); });
                } catch (const std::system_error &) {
                    break; // 资源不足时用已启动线程及调用线程完成所有任务。
                }
            }
        } catch (...) {
            close();
            throw;
        }
    }
    ~Pool() { close(); }
    Pool(const Pool &) = delete;
    Pool &operator=(const Pool &) = delete;

    template <class Work> void run(std::size_t count, Work &&work) {
        if (!count) return;
        std::unique_lock<std::mutex> batch(batch_mutex_, std::defer_lock);
        if (count == 1 || inside_batch || !batch.try_lock() || workers_.empty()) {
            if (batch.owns_lock()) batch.unlock();
            for (std::size_t i = 0; i < count; ++i) {
                poll_search_deadline();
                work(i);
            }
            return;
        }
        // 预先分配结果槽位，全部任务结束后按输入次序传递首个异常。
        std::vector<std::exception_ptr> errors(count);
        std::function<void(std::size_t)> wrapped = [&](std::size_t i) noexcept {
            try {
                poll_search_deadline();
                work(i);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        };
        {
            std::lock_guard<std::mutex> lock(mutex_);
            work_ = std::move(wrapped);
            count_ = count;
            next_.store(0, std::memory_order_relaxed);
            deadline_ = execution_deadline;
            pending_ = workers_.size();
            ++generation_;
        }
        changed_.notify_all();
        consume();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            finished_.wait(lock, [&] { return pending_ == 0; });
            work_ = {};
        }
        for (const auto &error : errors)
            if (error) std::rethrow_exception(error);
    }
};
inline Pool &pool() {
    static Pool instance;
    return instance;
}
} // namespace parallel_detail

template <class Work> void parallel_for(std::size_t count, Work &&work) {
    if (!count) return;
    if (count == 1) {
        poll_search_deadline();
        work(0);
        return;
    }
    parallel_detail::pool().run(count, std::forward<Work>(work));
}
} // namespace problem2
