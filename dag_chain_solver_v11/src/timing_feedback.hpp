#pragma once
#include "official_timing.hpp"

namespace dag_chain {
struct TimingFeedbackLimits {
    size_t previous_pipe_ops = 4;
    size_t closure_attempts = 32;
    size_t closure_nodes = 64;
    size_t closure_edges = 2048;
    size_t per_core_distance = 256;
};
// One deterministic neighborhood from a measured plan. Keep core assignments
// and co-location metadata exactly; groups need not execute contiguously.
// Timings only prioritize proposals: the caller must score each new whole plan.
vector<Solution> timing_feedback_candidates(const Graph &, int cores,
    const Solution &seed, const OfficialTimingResult &timing, size_t limit = 4,
    const TimingFeedbackLimits &limits = TimingFeedbackLimits{});
} // namespace dag_chain
