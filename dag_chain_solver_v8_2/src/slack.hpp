#pragma once
#include "model.hpp"

namespace dag_chain {
struct SlackStats {
    size_t groups_examined = 0;
    size_t moves_screened = 0;
    size_t replayed = 0;
    size_t candidates = 0;
};

// Heuristic helpers, not feasibility certificates or shared-DDR upper bounds.
// consumer_ready is the relevant consumer's time, never a global core maximum.
double slack_exposed_delay(double arrival, double consumer_ready);
double slack_uncontended_arrival(double producer_finish, i64 bytes, const Config &config);

// Releases a same-core preference by relocating a whole writer group. The input
// Graph and every original dependency remain unchanged. At most four globally
// reordered candidates are returned for the caller's full refinement/selection.
vector<Solution> slack_candidates(const Graph &graph, int cores, const Solution &seed,
                                 SlackStats *statistics = nullptr);
} // namespace dag_chain

