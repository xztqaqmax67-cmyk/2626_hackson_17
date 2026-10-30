#pragma once
#include "schedule_step3.hpp"
namespace problem2 {
struct SceneBEvaluationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct SceneBTasks {
    std::map<Id, PreparedTask> tasks;
    Json links, traffic, plan_view;
    Id cross_traffic = 0;
};
Id copy_traffic_bytes(const Json &graph);
SceneBTasks build_scene_b_tasks(const Json &graph, const Json &plan, double bandwidth,
                                const Capacity &capacity, bool skip_execution = false,
                                bool boundary_only = false);
void validate_execution(const std::map<Id, PreparedTask> &tasks, const Json &links);
Json evaluate_scene_b(const Json &graph, const Json &plan, double bandwidth,
                      const Capacity &capacity, double delay, Id max_iter = 1000000);
} // namespace problem2
