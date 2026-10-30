#include "spill_feedback.hpp"
#include "search_limits.hpp"
#include <optional>

namespace dag_chain {
namespace {
vector<int> stream_key(const Solution &solution, int cores) {
    vector<vector<int>> streams(cores);
    for (int v : solution.order) streams[solution.core[v]].push_back(v);
    vector<int> result;
    result.reserve(solution.order.size() + size_t(cores));
    for (const auto &stream : streams) {
        result.insert(result.end(), stream.begin(), stream.end());
        result.push_back(-1);
    }
    return result;
}

// Move a ready-able group before one allocation-triggering compute anchor.
// All still-unvisited original predecessors are included, never deleted. If the
// trigger is itself required, this move cannot avoid its allocation and is skipped.
optional<Solution> pull_before(const Graph &g, const Solution &seed,
                               const vector<int> &position, int anchor,
                               const vector<int> &roots) {
    if (anchor < 0 || anchor >= g.size() || roots.empty()) return nullopt;
    const int boundary = position[anchor];
    vector<unsigned char> selected(g.size());
    vector<int> stack = roots;
    size_t count = 0;
    while (!stack.empty()) {
        int v = stack.back(); stack.pop_back();
        if (v < 0 || v >= g.size()) return nullopt;
        if (position[v] < boundary || selected[v]) continue;
        if (v == anchor || size_t(position[v] - boundary) > search_limits::feedback_max_distance)
            return nullopt;
        selected[v] = 1;
        if (++count > search_limits::feedback_closure_nodes) return nullopt;
        for (int p : g.ops[v].pred) if (position[p] >= boundary) stack.push_back(p);
    }
    if (!count) return nullopt;
    Solution result = seed;
    result.order.clear();
    for (int v : seed.order) {
        if (v == anchor)
            for (int moved : seed.order) if (selected[moved]) result.order.push_back(moved);
        if (!selected[v]) result.order.push_back(v);
    }
    return result;
}

// Move the allocation trigger after a consumer. Any original successors crossed
// by this change move with it; encountering the consumer proves the move illegal.
optional<Solution> push_after(const Graph &g, const Solution &seed,
                              const vector<int> &position, int trigger, int consumer) {
    if (trigger < 0 || consumer < 0 || trigger >= g.size() || consumer >= g.size() ||
        position[trigger] >= position[consumer] ||
        size_t(position[consumer] - position[trigger]) > search_limits::feedback_max_distance)
        return nullopt;
    const int boundary = position[consumer];
    vector<unsigned char> selected(g.size());
    vector<int> stack{trigger};
    size_t count = 0;
    while (!stack.empty()) {
        int v = stack.back(); stack.pop_back();
        if (position[v] > boundary || selected[v]) continue;
        if (v == consumer) return nullopt;
        selected[v] = 1;
        if (++count > search_limits::feedback_closure_nodes) return nullopt;
        for (int w : g.ops[v].succ) if (position[w] <= boundary) stack.push_back(w);
    }
    Solution result = seed;
    result.order.clear();
    for (int v : seed.order) {
        if (!selected[v]) result.order.push_back(v);
        if (v == consumer)
            for (int moved : seed.order) if (selected[moved]) result.order.push_back(moved);
    }
    return result;
}
} // namespace

vector<Solution> spill_feedback_candidates(const Graph &g, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace,
                                           size_t *records_examined) {
    if (records_examined) *records_examined = 0;
    vector<Solution> result;
    if (!trace.valid || !search_limits::feedback_records ||
        !search_limits::feedback_candidates_per_round || !search_limits::feedback_closure_nodes)
        return result;
    validate(g, seed, cores);
    vector<int> position(g.size());
    for (int i = 0; i < g.size(); ++i) position[seed.order[i]] = i;
    vector<size_t> records(trace.records.size());
    iota(records.begin(), records.end(), size_t(0));
    stable_sort(records.begin(), records.end(), [&](size_t a, size_t b) {
        const auto &x = trace.records[a], &y = trace.records[b];
        if (x.transfer_bytes() != y.transfer_bytes()) return x.transfer_bytes() > y.transfer_bytes();
        return tie(x.core, x.tensor_id, x.trigger_step, x.next_use_step, a) <
               tie(y.core, y.tensor_id, y.trigger_step, y.next_use_step, b);
    });
    // Zero-byte events have no transfer saving, regardless of their relative order.
    records.erase(remove_if(records.begin(), records.end(), [&](size_t i) {
        return trace.records[i].transfer_bytes() <= 0;
    }), records.end());
    if (records.size() > search_limits::feedback_records) records.resize(search_limits::feedback_records);
    if (records_examined) *records_examined = records.size();
    set<vector<int>> seen;
    seen.insert(stream_key(seed, cores));

    auto add = [&](optional<Solution> candidate, const string &label) {
        if (!candidate || result.size() >= search_limits::feedback_candidates_per_round) return;
        validate(g, *candidate, cores);
        if (candidate->core != seed.core) throw runtime_error("feedback changed core assignment");
        if (!seen.insert(stream_key(*candidate, cores)).second) return;
        candidate->label = label;
        result.push_back(move(*candidate));
    };
    // Round-robin across costly records: first try bringing their next use close,
    // then try draining remaining local users, then delaying the trigger itself.
    for (int strategy = 0; strategy < 4 && result.size() < search_limits::feedback_candidates_per_round;
         ++strategy) {
        for (size_t record_index : records) {
            if (result.size() >= search_limits::feedback_candidates_per_round) break;
            const auto &record = trace.records[record_index];
            const int trigger = record.trigger_owner, next = record.next_owner;
            if (trigger < 0 || next < 0 || trigger >= g.size() || next >= g.size() ||
                position[trigger] >= position[next]) continue;
            vector<int> remaining;
            if (record.tensor_index >= 0 && record.tensor_index < int(g.tensors.size())) {
                for (int v : g.tensor_users[record.tensor_index])
                    if (seed.core[v] == record.core && position[v] > position[trigger]) remaining.push_back(v);
                stable_sort(remaining.begin(), remaining.end(), [&](int a, int b) {
                    return position[a] < position[b];
                });
            }
            if (remaining.empty()) remaining.push_back(next); // Synthetic direct-edge buffer.
            const string label = "spill_feedback_" + to_string(strategy) + "_" + to_string(record_index);
            if (strategy == 0) add(pull_before(g, seed, position, trigger, {next}), label);
            else if (strategy == 1) add(pull_before(g, seed, position, trigger, remaining), label);
            else add(push_after(g, seed, position, trigger,
                                strategy == 2 ? next : remaining.back()), label);
        }
    }
    return result;
}

Result improve_spill_order(const Graph &g, int cores, Result incumbent, FeedbackStats *statistics) {
    FeedbackStats stats;
    stats.before_cycles = stats.after_cycles = incumbent.refined.cycles;
    auto finish = [&]() {
        incumbent.candidates += stats.traced_candidates;
        incumbent.refined_candidates += stats.refined_candidates;
        if (statistics) *statistics = stats;
        return incumbent;
    };
    if (!search_limits::feedback_enabled) {
        stats.stop_reason = "disabled";
        return finish();
    }
    validate(g, incumbent.solution, cores);
    auto current = official_spill_trace(g, incumbent.solution, cores);
    stats.initial_trace_valid = current.valid;
    if (!current.valid) {
        stats.stop_reason = "initial_trace_invalid: " + current.error;
        return finish();
    }
    stats.before_spill_bytes = stats.after_spill_bytes = current.spill_bytes;
    stats.route_bytes = current.route_bytes;
    // Subsequent proposals only use complete spill records. Keep no redundant
    // copies of the large per-core event sequences during candidate ranking.
    current.cores.clear();
    stats.stop_reason = "round_limit";
    for (size_t round = 0; round < search_limits::feedback_rounds; ++round) {
        if (!current.spill_bytes) { stats.stop_reason = "no_spill"; break; }
        ++stats.rounds;
        size_t examined = 0;
        auto candidates = spill_feedback_candidates(g, cores, incumbent.solution, current, &examined);
        stats.records_examined += examined;
        stats.generated_candidates += candidates.size();
        if (candidates.empty()) { stats.stop_reason = "no_legal_reorder"; break; }
        struct Entry { Solution solution; OfficialSpillTrace trace; };
        vector<Entry> improved;
        for (auto &candidate : candidates) {
            auto trace = official_spill_trace(g, candidate, cores);
            ++stats.traced_candidates;
            // A gain cannot come from silently changing the boundary graph or
            // from merely changing which tensor spills with no aggregate saving.
            if (!trace.valid || trace.route_bytes != current.route_bytes ||
                trace.spill_bytes >= current.spill_bytes) continue;
            trace.cores.clear();
            improved.push_back({move(candidate), move(trace)});
        }
        stable_sort(improved.begin(), improved.end(), [](const Entry &a, const Entry &b) {
            return tie(a.trace.spill_bytes, a.solution.order) < tie(b.trace.spill_bytes, b.solution.order);
        });
        bool accepted = false;
        RefinedEstimate chosen_refined;
        size_t chosen = 0;
        for (size_t i = 0; i < min(improved.size(), search_limits::feedback_refine_candidates); ++i) {
            auto refined = refine_estimate(g, improved[i].solution, cores);
            ++stats.refined_candidates;
            if (!refined.valid || refined.cycles > incumbent.refined.cycles) continue;
            if (!accepted || tie(improved[i].trace.spill_bytes, refined.cycles, improved[i].solution.order) <
                             tie(improved[chosen].trace.spill_bytes, chosen_refined.cycles, improved[chosen].solution.order)) {
                accepted = true;
                chosen = i;
                chosen_refined = move(refined);
            }
        }
        if (!accepted) { stats.stop_reason = "no_cycle_safe_spill_reduction"; break; }
        incumbent.solution = move(improved[chosen].solution);
        incumbent.refined = move(chosen_refined);
        incumbent.estimate = evaluate_proxy(g, cores, Config{}, incumbent.solution, 0.03, nullptr);
        // This is the exact full replay of the accepted order, not the old victim
        // ranking. Its new records drive the next round without a stale trace.
        current = move(improved[chosen].trace);
        stats.after_spill_bytes = current.spill_bytes;
        stats.after_cycles = incumbent.refined.cycles;
        ++stats.accepted_rounds;
    }
    validate(g, incumbent.solution, cores);
    return finish();
}
} // namespace dag_chain
