#pragma once
#include "model.hpp"

namespace dag_chain {
// Local insertions around long per-core tensor lifetimes. Ownership is fixed;
// each proposal preserves all dependencies and the relative order of every
// operation except the single inserted operation. This is a neighborhood, not
// a spill proof: the caller must assess total traffic and execution time.
// Deadline cancellation throws without publishing partially built candidates.
vector<Solution> lifetime_candidates(const Graph &g, int cores, const Solution &base,
                                    Deadline *deadline = nullptr,
                                    size_t max_candidates = 24);
} // namespace dag_chain
