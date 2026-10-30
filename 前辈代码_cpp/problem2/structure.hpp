#pragma once
#include "pipeline.hpp"
namespace problem2 {
std::map<Id, Id> levels(const GraphInfo &graph);
std::pair<std::vector<Ids>, Ids> stage_branches(const GraphInfo &graph,
                                                std::optional<Id> width = {}, bool reverse = false,
                                                double deadline = 0);
Ids reverse_dfs_order(const GraphInfo &graph);
NamedPlans structure_seed_plans(const GraphInfo &graph, Id cores, const Settings &settings,
                                double deadline = 0);
} // namespace problem2
