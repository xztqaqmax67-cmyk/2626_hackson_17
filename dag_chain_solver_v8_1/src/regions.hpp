#pragma once
#include "chain.hpp"

namespace dag_chain {
// These are placement regions only. The original graph remains the execution DAG.
struct RegionPartition {
    vector<int> phase_of;
    vector<vector<vector<int>>> components;
};
struct RegionStats {
    size_t partitions = 0, split_phases = 0;
    size_t budget_vectors = 0, mask_assignments = 0;
};
RegionPartition make_region_partition(const Graph &g, int bands);
RegionPartition make_fork_join_partition(const Graph &g);
vector<vector<int>> enumerate_core_budgets(int blocks, int cores);
// Full-graph plans, with at most eight outputs; no layer barriers or removed edges.
vector<Solution> region_candidates(const Graph &g, int cores, const Solution &seed,
                                   RegionStats *stats = nullptr);
} // namespace dag_chain

