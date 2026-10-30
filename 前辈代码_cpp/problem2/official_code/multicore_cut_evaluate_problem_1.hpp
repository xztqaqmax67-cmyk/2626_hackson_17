#pragma once
#include "schedule_step3.hpp"
namespace problem2 {
struct SceneAEvaluationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct SceneATasks {
    std::map<Id, PreparedTask> tasks;
    Json traffic, plan_view;
    Id cross_traffic = 0;
};
SceneATasks build_scene_a_tasks(const Json &graph, const Json &plan, double bandwidth,
                                const Capacity &capacity);
Json evaluate_scene_a(const Json &graph, const Json &plan, double bandwidth,
                      const Capacity &capacity, double cross_wait, double same_wait,
                      Id max_iter = 1000000);
} // namespace problem2
