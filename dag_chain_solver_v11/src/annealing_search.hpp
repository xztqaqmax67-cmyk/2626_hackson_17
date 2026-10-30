#pragma once
#include "annealing.hpp"

namespace dag_chain {
enum class AnnealingStage { baseline, greedy_only, annealed };
struct AnnealingRun {
    size_t variant = 0, restart = 0;
    uint64_t seed = 0;
    AnnealingStats statistics;
};
struct AnnealingEvaluation {
    string phase, label, error;
    i64 cycles = -1, route_bytes = 0, spill_bytes = 0;
    bool improved = false;
};
struct AnnealingSearchStats {
    string stage, selected_label, error;
    bool baseline_valid = false, selected_new = false, assignment_changed = false;
    bool deadline_reached = false, diagnostics_refreshed = false;
    i64 baseline_cycles = -1, greedy_cycles = -1, after_cycles = -1;
    unsigned long long baseline_bytes = 0, greedy_bytes = 0, after_bytes = 0;
    size_t greedy_generated = 0, greedy_unique = 0, sa_generated = 0, sa_unique = 0;
    size_t proxy_evaluations = 0, exact_evaluations = 0, exact_invalid = 0;
    double baseline_seconds = 0, candidate_seconds = 0, annealing_seconds = 0;
    double greedy_exact_seconds = 0, sa_exact_seconds = 0, total_seconds = 0;
    vector<AnnealingRun> runs;
    vector<AnnealingEvaluation> exact_candidates;
};
// Preserve the freshly solved incumbent (which can be time-limited). New candidates follow
// actual greedy packing -> optional SA assignment -> full-DAG scheduling.
// Proxy energy ranks SA proposals only; final acceptance uses exact whole-plan
// cycles and total COPY bytes. No partially evaluated candidate can be output.
Result improve_annealing(const Graph &, int cores, Result incumbent,
    AnnealingStage stage = AnnealingStage::annealed, AnnealingSearchStats *stats = nullptr,
    Deadline *deadline = nullptr);
string annealing_search_stats_json(const AnnealingSearchStats &);
} // namespace dag_chain
