#pragma once
#include "chain.hpp"
#include "spill_feedback.hpp"

namespace dag_chain {
enum class V10Stage { v9 = 0, tensor_memory = 1, multi_feedback = 2,
                      exact_legacy = 3, exact_pool = 4, exact = 5 };
struct V10ExactCandidate {
    string phase, label, error;
    i64 cycles = -1, route_bytes = 0, spill_bytes = 0;
    bool improved = false;
};
struct V10Stats {
    size_t generated = 0, unique = 0, traced = 0, refined = 0;
    size_t feedback_starts = 0, feedback_replays = 0, feedback_refinements = 0;
    size_t exact_evaluations = 0, exact_invalid = 0;
    size_t tail_generated = 0, tail_evaluations = 0, tail_duplicates = 0, tail_accepted = 0;
    bool selected_new = false, exact_baseline_valid = false;
    double before_estimated_cycles = INF, after_estimated_cycles = INF;
    i64 before_exact_cycles = -1, after_exact_cycles = -1;
    i64 legacy_exact_cycles = -1, pool_exact_cycles = -1;
    unsigned long long before_total_bytes = 0, after_total_bytes = 0;
    double candidate_seconds = 0, feedback_seconds = 0, exact_seconds = 0;
    double tail_seconds = 0;
    string selected_label, stage, error;
    vector<string> exact_errors;
    vector<FeedbackStats> feedback;
    vector<V10ExactCandidate> exact_candidates;
};
// Run the bounded v10 portfolio AFTER the complete freshly computed v9 result.
// Stages 1/2 use the conservative old estimate+traffic guard. Stage 3 compares
// a shortlist against the v9 incumbent with the complete official model. The
// upgraded stages retain the old shortlist, then add exact-scored proposals.
Result improve_v10(const Graph &g, int cores, Result incumbent,
                   V10Stage stage = V10Stage::exact, V10Stats *statistics = nullptr);
string v10_stats_json(const V10Stats &statistics);
} // namespace dag_chain
