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
double simulate_events(const vector<TimingEvent> &events);
RefinedEstimate refine_estimate(const Graph &g, const Solution &s, int cores);
} // namespace dag_chain
