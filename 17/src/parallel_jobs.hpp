#pragma once
#include <array>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace dag_chain {
namespace detail {

struct JoinedThreads {
    std::vector<std::thread> threads;
    explicit JoinedThreads(int capacity) { threads.reserve(static_cast<std::size_t>(capacity)); }
    JoinedThreads(const JoinedThreads &) = delete;
    JoinedThreads &operator=(const JoinedThreads &) = delete;
    void join_all() {
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
    }
    ~JoinedThreads() { join_all(); }
};

// Work writes only its own indexed result. Index zero runs on the calling thread;
// at most three additional threads are created. Launch is injectable for tests.
template <class Work, class Launch> void run_independent_jobs(int count, Work work, Launch launch) {
    if (count < 1 || count > 4)
        throw std::invalid_argument("independent job count must be 1..4");
    std::array<std::exception_ptr, 4> errors{};
    auto execute = [&](int index) noexcept {
        try {
            work(index);
        } catch (...) {
            errors[index] = std::current_exception();
        }
    };
    // Declared after the captured state so exceptional unwinding joins before
    // destroying anything a worker can access.
    JoinedThreads workers(count - 1);
    for (int index = 1; index < count; ++index) {
        try {
            workers.threads.emplace_back(launch(execute, index));
        } catch (const std::system_error &) {
            // Resource pressure must not silently omit a candidate portfolio.
            execute(index);
        }
    }
    execute(0);
    workers.join_all();
    // Preserve the original serial error priority, regardless of finish order.
    for (int index = 0; index < count; ++index)
        if (errors[index])
            std::rethrow_exception(errors[index]);
}

template <class Work> void run_independent_jobs(int count, Work work) {
    run_independent_jobs(count, std::move(work),
                         [](auto execute, int index) { return std::thread(execute, index); });
}

} // namespace detail
} // namespace dag_chain
