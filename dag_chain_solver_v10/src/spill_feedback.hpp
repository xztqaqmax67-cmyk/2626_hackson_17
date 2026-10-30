#pragma once
#include "chain.hpp"
#include "official_spill.hpp"

namespace dag_chain {
struct FeedbackStats {
    i64 before_spill_bytes = 0, after_spill_bytes = 0, route_bytes = 0;
    double before_cycles = INF, after_cycles = INF;
    size_t rounds = 0, records_examined = 0, generated_candidates = 0;
    size_t traced_candidates = 0, refined_candidates = 0, accepted_rounds = 0;
    i64 after_route_bytes = 0;
    size_t records_productive = 0, seeds_traced = 0, mapping_generated = 0, neutral_retained = 0;
    size_t best_neutral_steps = 0;
    size_t rejected_anchor = 0, rejected_dependency = 0, rejected_distance = 0;
    size_t rejected_closure = 0, rejected_duplicate = 0, rejected_trace = 0;
    size_t rejected_traffic = 0, rejected_refine = 0, rejected_cycles = 0;
    array<size_t, 5> strategy_attempts{}, strategy_generated{};
    bool initial_trace_valid = false;
    string stop_reason;
};

// Optional per-call caps for v10's shared multi-start budget. A null pointer
// retains the exact v8/v9 defaults and does not change their search decisions.
struct FeedbackBudget {
    size_t replays = 48;
    size_t refinements = 12;
    size_t per_stage_refinements = 4;
};

// Development-visible bounded proposals from actual Step2 spill records.
// Core assignment is immutable. Every result is a complete original-DAG
// topological order, and duplicates are compared by submitted per-core streams.
vector<Solution> spill_feedback_candidates(const Graph &graph, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace,
                                           size_t *records_examined = nullptr,
                                           FeedbackStats *statistics = nullptr);

// Shared bounded search over seeds, mapping proposals and neutral order moves.
// Final selection prioritizes refined cycles, then exact total transfer bytes;
// neither exceeds the original incumbent. Step3 cycles remain approximate.
Result improve_spill_order(const Graph &graph, int cores, Result incumbent,
                           FeedbackStats *statistics = nullptr,
                           const vector<Solution> *alternatives = nullptr,
                           const FeedbackBudget *budget = nullptr);
string feedback_stats_json(const FeedbackStats &statistics);
} // namespace dag_chain
