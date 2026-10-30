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
} // namespace dag_chain

