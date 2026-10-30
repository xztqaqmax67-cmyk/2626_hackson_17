#pragma once
#include "common.hpp"
namespace problem2 {
class ChildProcess {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ChildProcess(const fs::path &executable, const std::vector<std::string> &args,
                 const fs::path &log);
    ~ChildProcess();
    std::optional<int> poll();
    std::optional<int> wait(double seconds);
    void stop();
};
struct WaitResult {
    std::optional<int> returncode;
    bool must_kill = false, extension_used = false, interrupted = false;
};
WaitResult wait_for_search(
    ChildProcess &child, double target, double hard, const fs::path &checkpoint,
    const std::function<bool()> &interrupted = []() { return false; });
std::map<Id, double> core_targets(const Ids &cores, double case_budget = 600,
                                  std::optional<double> time_limit = {});
std::tuple<double, double, double> case_slot_limits(double slot_start, double now, double target,
                                                    double case_deadline, double future_budget,
                                                    double grace_limit);
} // namespace problem2
