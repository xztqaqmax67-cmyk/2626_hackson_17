#pragma once
#include "official_spill.hpp"

namespace dag_chain {
struct TensorGroupLimits {
    size_t tensor_limit = 12;
    size_t evaluation_limit = 48;
    size_t per_direction = 2;
    size_t move_nodes = 64;
    size_t split_atoms = 8;
};
struct TensorGroupStats {
    size_t examined_tensors = 0, evaluated_moves = 0, rejected_size = 0;
    size_t rejected_noop = 0, rejected_duplicate = 0;
    array<size_t,3> evaluated_by_direction{}, generated_by_direction{};
    // Parallel to returned candidates. These count the complete constructed
    // route using destination-core sets (double precision), excluding spills.
    vector<double> route_delta, pipe_bound_delta, pressure_delta;
    vector<size_t> moved_nodes;
    vector<int> directions; // 0 Tensor gather, 1 consumer-core gather, 2 split.
};
// Generate a bounded deterministic portfolio, never accept an output here.
// The caller must replay exact Step2 and assess cycles. Existing algorithmic
// placement groups may be split; complete mandatory writer atoms never split.
vector<Solution> tensor_group_candidates(const Graph &graph, int cores, const Solution &seed,
    const OfficialSpillTrace *trace = nullptr, TensorGroupStats *statistics = nullptr,
    const TensorGroupLimits &limits = TensorGroupLimits{});
} // namespace dag_chain
