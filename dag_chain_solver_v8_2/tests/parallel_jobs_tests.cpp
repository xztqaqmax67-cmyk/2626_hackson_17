#include "parallel_jobs.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

namespace {
using dag_chain::detail::run_independent_jobs;

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Gate {
    std::mutex mutex;
    std::condition_variable condition;
    bool released = false;
    void open() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            released = true;
        }
        condition.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        require(condition.wait_for(lock, std::chrono::seconds(5), [&] { return released; }),
                "synchronization gate timed out");
    }
};

void indexed_jobs_and_calling_thread() {
    const auto caller = std::this_thread::get_id();
    std::array<std::atomic<int>, 4> counts;
    for (auto &count : counts)
        count.store(0);
    std::array<int, 4> values{};
    std::array<std::thread::id, 4> owners{};
    run_independent_jobs(4, [&](int index) {
        require(index >= 0 && index < 4, "job index outside its result slots");
        require(counts[index].fetch_add(1) == 0, "job executed more than once");
        values[index] = 100 + index;
        owners[index] = std::this_thread::get_id();
    });
    for (int index = 0; index < 4; ++index) {
        require(counts[index].load() == 1 && values[index] == 100 + index,
                "indexed job result missing or reordered");
        require((owners[index] == caller) == (index == 0),
                "job zero must use the caller and other jobs must use workers");
    }
    int serial_calls = 0;
    run_independent_jobs(1, [&](int index) {
        require(index == 0 && std::this_thread::get_id() == caller,
                "single job should run directly on the caller");
        ++serial_calls;
    });
    require(serial_calls == 1, "single job executed an incorrect number of times");
}

void joins_before_rethrowing_work_error() {
    Gate main_started;
    std::atomic<int> finished_workers{0};
    bool caught = false;
    try {
        run_independent_jobs(4, [&](int index) {
            if (index == 0) {
                main_started.open();
                throw std::runtime_error("main job failure");
            }
            main_started.wait();
            finished_workers.fetch_add(1);
        });
    } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "main job failure";
        require(finished_workers.load() == 3, "work error escaped before workers finished");
    }
    require(caught, "main job exception was lost or replaced");
}

void deterministic_error_priority() {
    Gate later_job_finished;
    std::array<std::atomic<int>, 4> counts;
    for (auto &count : counts)
        count.store(0);
    bool caught = false;
    try {
        run_independent_jobs(4, [&](int index) {
            counts[index].fetch_add(1);
            if (index == 3)
                throw std::runtime_error("failure from job 3");
            if (index == 1) {
                later_job_finished.wait();
                throw std::runtime_error("failure from job 1");
            }
        }, [&](auto execute, int index) -> std::thread {
            return std::thread([execute, index, &later_job_finished] {
                execute(index);
                // execute has already captured job 3's exception before job 1
                // is released. Error arrival order is deliberately reversed.
                if (index == 3)
                    later_job_finished.open();
            });
        });
    } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "failure from job 1";
    }
    require(caught, "exceptions must be selected by job index, not finish order");
    for (const auto &count : counts)
        require(count.load() == 1, "a work exception omitted another job");
}

void launch_failure_runs_missing_job_synchronously() {
    const auto caller = std::this_thread::get_id();
    std::array<std::atomic<int>, 4> counts;
    for (auto &count : counts)
        count.store(0);
    std::array<std::thread::id, 4> owners{};
    std::array<int, 4> launches{};
    run_independent_jobs(4, [&](int index) {
        require(counts[index].fetch_add(1) == 0, "fallback duplicated a job");
        owners[index] = std::this_thread::get_id();
    }, [&](auto execute, int index) -> std::thread {
        ++launches[index];
        if (index == 2)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        return std::thread(execute, index);
    });
    for (int index = 0; index < 4; ++index) {
        require(counts[index].load() == 1, "launch failure omitted a job");
        require(launches[index] == (index == 0 ? 0 : 1), "unexpected launch count");
    }
    require(owners[0] == caller && owners[2] == caller,
            "failed launch must fall back to the calling thread");
    require(owners[1] != caller && owners[3] != caller,
            "successful workers should survive another launch's failure");
}

void unexpected_launch_error_joins_existing_workers() {
    Gate release_worker;
    std::atomic<bool> worker_finished{false};
    bool caught = false;
    try {
        run_independent_jobs(4, [&](int index) {
            require(index == 1, "unexpected work after a nonrecoverable launch error");
            release_worker.wait();
            worker_finished.store(true);
        }, [&](auto execute, int index) -> std::thread {
            if (index == 2) {
                release_worker.open();
                throw std::runtime_error("unexpected launch failure");
            }
            return std::thread(execute, index);
        });
    } catch (const std::runtime_error &error) {
        caught = std::string(error.what()) == "unexpected launch failure";
        require(worker_finished.load(), "launch error escaped before an existing worker finished");
    }
    require(caught, "unexpected launch error was swallowed or replaced");
}

void invalid_counts_do_not_execute_work() {
    for (int count : {0, 5}) {
        std::atomic<bool> executed{false};
        bool caught = false;
        try {
            run_independent_jobs(count, [&](int) { executed.store(true); });
        } catch (const std::invalid_argument &) {
            caught = true;
        }
        require(caught && !executed.load(), "invalid job count must fail before launching work");
    }
}
} // namespace

int main() {
    try {
        indexed_jobs_and_calling_thread();
        joins_before_rethrowing_work_error();
        deterministic_error_priority();
        launch_failure_runs_missing_job_synchronously();
        unexpected_launch_error_joins_existing_workers();
        invalid_counts_do_not_execute_work();
        std::cout << "parallel jobs: indexed results, caller participation, deterministic errors, "
                     "joining and launch fallback passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

