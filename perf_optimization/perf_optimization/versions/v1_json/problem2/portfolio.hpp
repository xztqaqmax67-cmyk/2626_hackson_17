#pragma once
#include "core.hpp"
namespace problem2 {
// 字典序目标：先比较完整评分的 makespan，相同时再比较额外搬运字节数。
using Objective = std::pair<double, Id>;
struct SearchRow {
    Objective score;
    Plan plan;
    Json result;
    std::string label;
};
Objective objective(const Json &result);
Ids assignment_key(const GraphInfo &graph, const Plan &plan);
std::string seed_family(const std::string &label);
std::vector<SearchRow> diverse_rows(const GraphInfo &graph, std::vector<SearchRow> rows, Id limit);
std::vector<SearchRow> polishing_seeds(const GraphInfo &graph, const std::vector<SearchRow> &rows,
                                       Id limit = 8);
struct Route {
    // 每条路线独立维护精英池和已尝试方案，避免一条路线覆盖其它结构的探索机会。
    const GraphInfo *graph;
    std::string name;
    bool reference = false, compact = false;
    SearchRow best;
    double initial = 0, seconds = 0, repair_cost = 0;
    Id repair_failures = 0, repair_opportunities = 0;
    Json stats;
    std::vector<SearchRow> pool; // 保存较好且有差异的方案，停滞时可换一个中心继续搜索。
    std::set<std::string> visited, evaluated, repaired;
    std::set<Ids> reordered, activated;
    Route(const GraphInfo &graph, std::string name, const std::vector<SearchRow> &rows,
          bool reference = false, bool compact = false);
    int offer(const SearchRow &row);
    std::optional<SearchRow> next_center();
    bool repair_allowed(const std::string &key);
    double weight() const;
};
Route *select_route(const std::vector<Route *> &routes);
} // namespace problem2
