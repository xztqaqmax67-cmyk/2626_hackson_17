#pragma once
#include "chain.hpp"
#include <cstdint>

namespace dag_chain {
struct AnnealingLimits {
    // Fixed work, not a wall-clock timeout. A rejected proposal also uses a step.
    size_t proposals = 1024;
    size_t move_nodes = 128;       // Entire move/swap closure, never partial groups.
    size_t node_touches = 65536;   // Forward node moves; undo costs at most the same.
    size_t retained = 4;          // Greedy, best energy, then distinct route/pipe states.
    unsigned swap_percent = 25;
    uint64_t seed = 0x3d25c19b7a6e104fULL;
    double route_weight = 0.03;
    double balance_weight = 0.01;
    // T is measured in proxy-energy units. Geometric cooling scales these
    // fractions by max(1, initial energy); larger fractions admit worse moves.
    double start_temperature = 0.02;
    double end_temperature = 0.0001;
};
struct AnnealingStats {
    bool deadline_reached = false;
    size_t proposals = 0, evaluated = 0, move_proposals = 0, swap_proposals = 0;
    size_t accepted = 0, uphill_accepted = 0, rejected = 0;
    size_t skipped_size = 0, skipped_budget = 0, skipped_noop = 0;
    size_t touched_nodes = 0, undo_nodes = 0, rebasings = 0, bundles = 0;
    double initial_energy = 0, best_energy = 0, final_energy = 0;
    double initial_temperature = 0, final_temperature = 0;
    double initial_route_bytes = 0, best_route_bytes = 0;
    double initial_pipe_bound = 0, best_pipe_bound = 0;
};
struct AnnealingCandidate {
    vector<int> assignment;
    double energy = 0, route_bytes = 0, pipe_bound = 0;
    string origin;
};
struct AnnealingResult {
    vector<int> best_assignment;
    vector<int> placement_group;
    vector<AnnealingCandidate> candidates;
};
// The caller supplies the actual decompose -> greedy-pack result. Complete
// input bundles and mandatory writer groups are closed transitively; an input
// assignment that already splits such a group is rejected. No DAG contraction
// or scheduling occurs here. Returned assignments need original-DAG scheduling
// and official scoring; this energy excludes spilling, sync and pipe overlap.
AnnealingResult anneal_assignment(const Graph &, const Decomposition &, int cores,
    const vector<int> &greedy, const AnnealingLimits &limits = AnnealingLimits{},
    AnnealingStats *statistics = nullptr, Deadline *deadline = nullptr);
} // namespace dag_chain
