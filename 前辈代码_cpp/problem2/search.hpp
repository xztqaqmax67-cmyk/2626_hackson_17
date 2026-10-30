#pragma once
#include "core.hpp"
namespace problem2 {
struct Candidate {
    std::string kind, label;
    Plan plan;
    bool needs_repair = false;
};
struct SearchOptions {
    double time_limit = DEFAULT_CASE_TIME_LIMIT, feasibility_grace = 0;
    fs::path warm_start, inherited_checkpoint, resume_checkpoint;
    std::string ablation = "full";
    Id max_evaluations = 0;
    bool compact = false, export_results = true;
};
double pipe_bound(const GraphInfo &graph, const Plan &plan);
std::vector<Candidate> neighborhoods(const GraphInfo &graph, const Plan &plan, const Json &profile,
                                     const Json &official_result, int family,
                                     const Settings &settings, bool no_group = false,
                                     bool no_repair = false);
Json solve(const fs::path &graph, Id cores, const fs::path &config, const fs::path &output,
           const SearchOptions &options = {});
} // namespace problem2
