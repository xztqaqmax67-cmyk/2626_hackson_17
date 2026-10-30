#pragma once
#include "core.hpp"
namespace problem2 {
// 就绪窗口只从依赖已满足的操作中挑选候选，权衡流水线完成时间和核间负载。
using NamedPlans = std::vector<std::pair<std::string, Plan>>;
// 返回 false 即停止该来源，避免构造本轮不会评估的剩余种子。
using SeedVisitor = std::function<bool(std::string, Plan)>;
void visit_pipeline_seeds(const GraphInfo &graph, Id cores, const Settings &settings,
                          double deadline, const SeedVisitor &visit);
Plan operation_plan(const GraphInfo &graph, Id cores, const Settings &settings, double balance = 0,
                    Id window = 1, const std::string &mode = "critical",
                    const std::optional<std::map<Id, Id>> &assignment = {}, double deadline = 0,
                    const std::map<Id, double> &starts = {});
NamedPlans pipeline_seed_plans(const GraphInfo &graph, Id cores, const Settings &settings,
                               double deadline = 0);
NamedPlans reorder_plans(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                         const Json &result = Json(), Id limit = 4);
} // namespace problem2
