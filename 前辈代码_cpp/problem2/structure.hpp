#pragma once
#include "pipeline.hpp"
namespace problem2 {
void visit_structure_seeds(const GraphInfo &graph, Id cores, const Settings &settings,
                           double deadline, const SeedVisitor &visit);
// 结构种子从层次、分支及反向遍历中提取不同的初始分块；不替代完整评分。
std::map<Id, Id> levels(const GraphInfo &graph);
std::pair<std::vector<Ids>, Ids> stage_branches(const GraphInfo &graph,
                                                std::optional<Id> width = {}, bool reverse = false,
                                                double deadline = 0);
Ids reverse_dfs_order(const GraphInfo &graph);
NamedPlans structure_seed_plans(const GraphInfo &graph, Id cores, const Settings &settings,
                                double deadline = 0);
} // namespace problem2
