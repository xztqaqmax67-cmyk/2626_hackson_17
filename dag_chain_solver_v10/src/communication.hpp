#pragma once
#include "chain.hpp"
#include "spill_feedback.hpp"

namespace dag_chain {
struct CommunicationStats {
    size_t generated = 0, unique_candidates = 0, refined_candidates = 0;
    size_t traced_candidates = 0, rejected_refine = 0, rejected_trace = 0;
    size_t rejected_traffic = 0, rejected_cycles = 0, feedback_runs = 0;
    size_t oversized_traffic_explored = 0;
    size_t selected_groups = 0, selected_max_group_nodes = 0;
    bool baseline_trace_valid = false, selected_new = false;
    double before_cycles = INF, after_cycles = INF;
    i64 before_route_bytes = 0, before_spill_bytes = 0;
    i64 after_route_bytes = 0, after_spill_bytes = 0;
    string selected_label;
    FeedbackStats feedback;
};

// Greedy maximum-transfer path cover, followed by bounded minimum-cut-cost
// segmentation. Bundles are co-location constraints, never atomic operations.
// Optional footprint guard is a conservative search heuristic: the union of
// touched tensors is not the concurrent live memory of an interleaved group.
Decomposition communication_decompose(const Graph &graph, int cores, double scale,
                                      bool footprint_guard = true);
// At most (cores-1)*variant_limit*2 plans, with full DAG-ready scheduling.
vector<Solution> communication_candidates(const Graph &graph, int cores,
                                          CommunicationStats *statistics = nullptr);
// Preserve the supplied complete v8 incumbent. Only output a strict improvement
// in (estimated cycles, exact total COPY), with neither metric above that entry.
Result improve_communication_groups(const Graph &graph, int cores, Result incumbent,
                                    CommunicationStats *statistics = nullptr);
string communication_stats_json(const CommunicationStats &statistics);
} // namespace dag_chain
