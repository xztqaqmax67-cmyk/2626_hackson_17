#pragma once
#include "core.hpp"
namespace problem2 {
Json instance_fingerprint(const fs::path &graph, const fs::path &config);
Plan lift_plan(const GraphInfo &graph, const Plan &plan, Id cores);
std::pair<Plan, Json> load_inherited(const GraphInfo &graph, const fs::path &path, Id cores,
                                     const Json &instance, bool allow_same_core = false);
} // namespace problem2
