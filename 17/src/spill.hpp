#pragma once
#include "model.hpp"

namespace dag_chain {
// Generic, bounded proposal generator. It changes only the assignment and a
// topological operation order; official allocation/eviction rules are untouched.
// The caller must preserve the incumbent and refine these proposals before use.
vector<Solution> spill_candidates(const Graph &graph, int cores, const Solution &seed);
} // namespace dag_chain
