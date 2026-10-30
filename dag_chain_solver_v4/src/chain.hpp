#pragma once
#include "model.hpp"
#include "refine.hpp"

namespace dag_chain {
struct Decomposition {
    // parent is the latest-finishing predecessor in an unlimited-resource sketch.
    // next retains at most one selected successor, so paths do not fork.
    vector<int> parent, next;
    vector<double> earliest_finish;
    vector<vector<int>> paths, bundles;
    vector<int> bundle_of;
};
Decomposition decompose(const Graph &graph, int cores, double segment_scale,
                        double communication_weight = 0);
Solution repair_writer_order(const Graph &graph, const Solution &solution,
                             const vector<int> &partial_write_victims);
struct Result {
    Solution solution;
    Metrics estimate;
    RefinedEstimate refined;
    size_t candidates = 0, paths = 0, bundles = 0;
    size_t repair_attempts = 0;
    size_t refined_candidates = 0;
    int generated_for_cores = 0;
};
Result solve(const Graph &graph, int cores, bool baseline_only = false);
} // namespace dag_chain
