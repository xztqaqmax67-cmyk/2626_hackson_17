#pragma once
#include "official_spill.hpp"

namespace dag_chain {
// Small placement neighborhoods selected from actual Step2 spill events. Every
// move includes entire writer groups and preserves the original DAG order.
// Costs here rank proposals only; callers must fully replay and refine them.
vector<Solution> spill_mapping_candidates(const Graph &graph, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace);
} // namespace dag_chain
