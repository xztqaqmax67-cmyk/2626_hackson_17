#pragma once
#include "model.hpp"

namespace dag_chain {
// Port of Scene B singleton-task construction, Step1 and Step2 spill decisions.
// This trace does not claim to reproduce Step3 execution cycles or physical stalls.
struct OfficialTraceOp {
    i64 id = -1;
    int owner = -1; // Dense original compute-op index, including for boundary COPYs.
    string kind;
};
struct OfficialCoreTrace {
    int core = 0;
    vector<OfficialTraceOp> sequence;
    array<i64, 2> peak{};
    size_t overflow_checks = 0;
};
struct OfficialSpillRecord {
    int core = 0, tensor_index = -1; // -1 denotes a synthetic direct-edge buffer.
    i64 tensor_id = -1, size = 0;
    int trigger_step = -1, prev_use_step = -1, next_use_step = -1;
    int trigger_owner = -1, prev_owner = -1, next_owner = -1;
    i64 trigger_op = -1, prev_use_op = -1, next_use_op = -1;
    int memory = 0;
    bool spill_out_copies_data = false;
    i64 spill_out_id = -1, spill_in_id = -1, backing_tid = -1;
    i64 from_tid = -1, to_tid = -1;
    int version = 0;
    string backing_source;
    i64 transfer_bytes() const; // Throws instead of overflowing signed arithmetic.
};
struct OfficialSpillTrace {
    bool valid = false;
    string error;
    i64 route_bytes = 0, spill_bytes = 0;
    vector<OfficialCoreTrace> cores;
    vector<OfficialSpillRecord> records;
};
OfficialSpillTrace official_spill_trace(const Graph &graph, const Solution &solution, int cores);
string official_spill_json(const OfficialSpillTrace &trace);

// Ordered, physical-incarnation IR shared by exact Step2 and Step3. Edges keep
// the official insertion order; this also determines CPython set insertion ties.
struct OfficialExecutionTensor { i64 id = -1, size = 0; int memory = -1; };
struct OfficialExecutionOp {
    i64 id = -1, cycles = 1;
    int owner = -1, pipe = 0;
    string kind;
};
struct OfficialExecutionGraph {
    vector<OfficialExecutionTensor> tensors;
    vector<OfficialExecutionOp> ops;
    vector<pair<i64, i64>> edges;
    vector<i64> sequence;
};
struct OfficialCrossLink {
    int source_core = 0, target_core = 0;
    i64 source_copy_out_id = -1, target_copy_in_id = -1;
};
struct OfficialSceneIR {
    bool valid = false;
    string error;
    i64 route_bytes = 0, spill_bytes = 0;
    vector<OfficialExecutionGraph> tasks;
    vector<OfficialCrossLink> cross_links;
};
OfficialSceneIR official_execution_graph(const Graph &, const Solution &, int cores);
} // namespace dag_chain
