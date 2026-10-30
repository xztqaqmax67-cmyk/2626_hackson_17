#pragma once
#include "common.hpp"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace problem2 {
// C++17 可暂停生成器：休眠线程保存普通函数的局部变量和调用栈。
// next() 唤醒生产者，yield(value) 交回控制权；二者交替执行，不并发搜索。
// 销毁时取消并 join，确保生产者退出后才释放捕获的外部变量。
// 仅允许单个调用者顺序调用 next()；同时传递搜索用的线程局部截止时间。
template <class T> class Generator {
public:
    using Yield = std::function<void(T)>;
    using Producer = std::function<void(const Yield &)>;

private:
    struct Cancelled {};
    struct State {
        std::mutex mutex;
        std::condition_variable changed;
        Producer produce;
        std::thread worker;
        std::optional<T> value;
        std::exception_ptr error;
        bool resume = false, ready = false, done = false, stop = false;
        double deadline = 0;
        explicit State(Producer producer) : produce(std::move(producer)) {}
    };
    std::unique_ptr<State> state_;

    static void produce(State &state) {
        try {
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.changed.wait(lock, [&]() { return state.resume || state.stop; });
                if (state.stop) {
                    throw Cancelled{};
                }
                execution_deadline = state.deadline;
            }
            state.produce([&](T value) {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.value = std::move(value);
                state.deadline = execution_deadline;
                state.ready = true;
                state.resume = false;
                state.changed.notify_all();
                state.changed.wait(lock, [&]() { return state.resume || state.stop; });
                if (state.stop) {
                    throw Cancelled{};
                }
                execution_deadline = state.deadline;
            });
        } catch (const Cancelled &) {
            // 正常销毁只展开局部变量，不再推进候选搜索。
        } catch (...) {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.error = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(state.mutex);
        state.deadline = execution_deadline;
        state.done = true;
        state.changed.notify_all();
    }

    void close() {
        if (!state_ || !state_->worker.joinable()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->stop = true;
            state_->changed.notify_all();
        }
        state_->worker.join();
    }

public:
    explicit Generator(Producer producer) : state_(std::make_unique<State>(std::move(producer))) {}
    Generator(Generator &&) noexcept = default;
    Generator &operator=(Generator &&other) noexcept {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
        }
        return *this;
    }
    Generator(const Generator &) = delete;
    ~Generator() {
        close();
    }

    std::optional<T> next() {
        if (!state_) {
            return {};
        }
        auto &state = *state_;
        std::unique_lock<std::mutex> lock(state.mutex);
        if (state.done) {
            return {};
        }
        if (!state.worker.joinable()) {
            state.worker = std::thread([&state]() { produce(state); });
        }
        state.deadline = execution_deadline;
        state.ready = false;
        state.resume = true;
        state.changed.notify_all();
        state.changed.wait(lock, [&]() { return state.ready || state.done; });
        execution_deadline = state.deadline;
        if (state.error) {
            std::rethrow_exception(state.error);
        }
        return std::exchange(state.value, std::nullopt);
    }
};
} // namespace problem2
