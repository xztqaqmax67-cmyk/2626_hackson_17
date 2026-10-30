#pragma once
#include "official_spill.hpp"

namespace dag_chain {
struct OfficialOpTiming { i64 id = -1, start = 0, end = 0; };
struct OfficialMemoryDependency {
    i64 source = -1, target = -1, reused_bytes = 0;
    vector<i64> previous_tensor_ids;
    string kind;
    vector<string> positions;
};
struct OfficialCoreTiming {
    int core = 0;
    i64 local_cycles = 0;
    array<i64, 2> peak{};
    array<vector<i64>, 5> pipe_orders;
    vector<OfficialMemoryDependency> memory_dependencies;
    vector<OfficialOpTiming> local_timeline, timeline;
};
struct OfficialTimingResult {
    bool valid = false;
    string error;
    i64 cycles = 0, route_bytes = 0, spill_bytes = 0;
    vector<OfficialCoreTiming> cores;
};
// Singleton Scene B, fixed official hardware constants. Integer-set iteration
// follows 64-bit CPython 3.11.11, the pinned reference evaluator runtime.
OfficialTimingResult official_timing(const Graph &, const Solution &, int cores);
OfficialTimingResult official_timing(const OfficialSceneIR &);
string official_timing_json(const OfficialTimingResult &);
// Public for differential tests of arbitrary IDs, including Python hash collisions.
vector<i64> official_python_int_set_order(const vector<i64> &);
} // namespace dag_chain
