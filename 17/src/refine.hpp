#pragma once
#include "model.hpp"

namespace dag_chain {
// Development-visible event representation. DDR work is exclusive-bandwidth
// cycles; simultaneous DDR operations share one 60-byte/cycle link.
struct TimingEvent {
    int core = 0, pipe = 0;
    double work = 0;
    bool ddr = false;
    vector<pair<int, double>> predecessors;
};
struct RefinedEstimate {
    double cycles = INF, route = 0, spill = 0;
    array<double, 2> peak{};
    bool valid = false;
    size_t events = 0;
};
// Null preserves the original complete v5 estimator. A supplied deadline throws
// SearchExpired; an interrupted event/model stream is never accepted as a plan.
double simulate_events(const vector<TimingEvent> &events, Deadline *deadline = nullptr);
RefinedEstimate refine_estimate(const Graph &g, const Solution &s, int cores,
                                Deadline *deadline = nullptr);
} // namespace dag_chain
