#pragma once
#include "model.hpp"

namespace dag_chain {
struct Decomposition {
    // parent is the latest-finishing predecessor in an unlimited-resource sketch.
    // next retains at most one selected successor, so paths do not fork.
    vector<int> parent, next;
    vector<vector<int>> paths, bundles;
    vector<int> bundle_of;
};
Decomposition decompose(const Graph &graph, int cores, double segment_scale,
                        double communication_weight = 0);
struct Result {
    Solution solution;
    Metrics estimate;
    size_t candidates = 0, paths = 0, bundles = 0;
};
Result solve(const Graph &graph, int cores, bool baseline_only = false);
} // namespace dag_chain
