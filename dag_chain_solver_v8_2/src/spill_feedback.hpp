#pragma once
#include "chain.hpp"
#include "official_spill.hpp"

namespace dag_chain {
struct FeedbackStats {
    i64 before_spill_bytes = 0, after_spill_bytes = 0, route_bytes = 0;
    double before_cycles = INF, after_cycles = INF;
    size_t rounds = 0, records_examined = 0, generated_candidates = 0;
    size_t traced_candidates = 0, refined_candidates = 0, accepted_rounds = 0;
    bool initial_trace_valid = false;
    string stop_reason;
};

// Development-visible bounded proposals from actual Step2 spill records.
// Core assignment is immutable. Every result is a complete original-DAG
// topological order, and duplicates are compared by submitted per-core streams.
vector<Solution> spill_feedback_candidates(const Graph &graph, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace,
                                           size_t *records_examined = nullptr);

// Run after the complete incumbent search. Accept only strict exact-Step2 spill
// reductions with unchanged boundary traffic and non-increasing refined cycles.
// Refined cycles remain an estimate; this is not a port of official Step3.
Result improve_spill_order(const Graph &graph, int cores, Result incumbent,
                           FeedbackStats *statistics = nullptr);
} // namespace dag_chain

