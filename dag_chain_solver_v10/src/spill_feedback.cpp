#include "spill_feedback.hpp"
#include "search_limits.hpp"
#include "spill_mapping.hpp"
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

struct Positions {
    vector<int> global;
    vector<vector<int>> before;
    Positions(const Solution &s, int cores) : global(s.order.size()),
        before(cores, vector<int>(s.order.size() + 1)) {
        for (size_t i = 0; i < s.order.size(); ++i) {
            global[s.order[i]] = int(i);
            for (int c = 0; c < cores; ++c)
                before[c][i + 1] = before[c][i] + (s.core[s.order[i]] == c);
        }
    }
    bool distant(const Solution &s, int v, int boundary) const {
        return size_t(abs(before[s.core[v]][global[v]] - before[s.core[v]][boundary])) >
               search_limits::feedback_max_distance;
    }
};

// Global DAG closure is mandatory even though distance is measured per core.
optional<Solution> pull_before(const Graph &g, const Solution &seed,
                              const Positions &pos, int anchor, const vector<int> &roots,
                              FeedbackStats &stats) {
    const int boundary = pos.global[anchor];
    vector<unsigned char> selected(g.size());
    vector<int> stack = roots;
    size_t count = 0;
    while (!stack.empty()) {
        int v = stack.back(); stack.pop_back();
        if (pos.global[v] < boundary || selected[v]) continue;
        if (v == anchor) { ++stats.rejected_dependency; return nullopt; }
        if (pos.distant(seed, v, boundary)) { ++stats.rejected_distance; return nullopt; }
        selected[v] = 1;
        if (++count > search_limits::feedback_closure_nodes) { ++stats.rejected_closure; return nullopt; }
        for (int p : g.ops[v].pred) if (pos.global[p] >= boundary) stack.push_back(p);
    }
    if (!count) { ++stats.rejected_anchor; return nullopt; }
    Solution result = seed;
    result.order.clear();
    for (int v : seed.order) {
        if (v == anchor)
            for (int moved : seed.order) if (selected[moved]) result.order.push_back(moved);
        if (!selected[v]) result.order.push_back(v);
    }
    return result;
}

optional<Solution> push_after(const Graph &g, const Solution &seed,
                             const Positions &pos, int trigger, int consumer,
                             FeedbackStats &stats) {
    if (pos.global[trigger] >= pos.global[consumer]) { ++stats.rejected_anchor; return nullopt; }
    const int boundary = pos.global[consumer];
    vector<unsigned char> selected(g.size());
    vector<int> stack{trigger};
    size_t count = 0;
    while (!stack.empty()) {
        int v = stack.back(); stack.pop_back();
        if (pos.global[v] > boundary || selected[v]) continue;
        if (v == consumer) { ++stats.rejected_dependency; return nullopt; }
        if (pos.distant(seed, v, boundary)) { ++stats.rejected_distance; return nullopt; }
        selected[v] = 1;
        if (++count > search_limits::feedback_closure_nodes) { ++stats.rejected_closure; return nullopt; }
        for (int w : g.ops[v].succ) if (pos.global[w] <= boundary) stack.push_back(w);
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

// Compare sums without signed overflow. Each trace component is nonnegative i64.
using Bytes = unsigned long long;
Bytes total(const OfficialSpillTrace &t) { return Bytes(t.route_bytes) + Bytes(t.spill_bytes); }
} // namespace

vector<Solution> spill_feedback_candidates(const Graph &g, int cores,
                                           const Solution &seed,
                                           const OfficialSpillTrace &trace,
                                           size_t *records_examined,
                                           FeedbackStats *statistics) {
    FeedbackStats discarded;
    auto &stats = statistics ? *statistics : discarded;
    if (records_examined) *records_examined = 0;
    vector<Solution> result;
    if (!trace.valid || !search_limits::feedback_records ||
        !search_limits::feedback_candidates_per_round || !search_limits::feedback_closure_nodes)
        return result;
    validate(g, seed, cores);
    Positions pos(seed, cores);
    vector<size_t> records;
    for (size_t i = 0; i < trace.records.size(); ++i)
        if (trace.records[i].transfer_bytes() > 0) records.push_back(i);
    stable_sort(records.begin(), records.end(), [&](size_t a, size_t b) {
        const auto &x = trace.records[a], &y = trace.records[b];
        if (x.transfer_bytes() != y.transfer_bytes()) return x.transfer_bytes() > y.transfer_bytes();
        return tie(x.core, x.tensor_id, x.trigger_step, x.next_use_step, a) <
               tie(y.core, y.tensor_id, y.trigger_step, y.next_use_step, b);
    });
    // Diversity is applied BEFORE the bounded scan. Equal quotas across cores,
    // tensors and trigger regions prevent repeated large victims monopolizing it.
    vector<size_t> core_count(cores);
    map<pair<int, i64>, size_t> tensor_group;
    map<pair<int, int>, size_t> region_group;
    vector<size_t> tensor_of(records.size()), region_of(records.size());
    for (size_t rank = 0; rank < records.size(); ++rank) {
        const auto &r = trace.records[records[rank]];
        const int region = r.trigger_step / int(max(size_t(1), search_limits::feedback_trigger_region));
        tensor_of[rank] = tensor_group.emplace(make_pair(r.core,r.tensor_id), tensor_group.size()).first->second;
        region_of[rank] = region_group.emplace(make_pair(r.core,region), region_group.size()).first->second;
    }
    vector<size_t> tensor_count(tensor_group.size()), region_count(region_group.size());
    vector<unsigned char> visited(records.size());
    set<vector<int>> seen;
    seen.insert(stream_key(seed, cores));
    array<vector<Solution>, 5> buckets;
    size_t scanned = 0, productive = 0;
    while (scanned < min(records.size(), search_limits::feedback_record_scan) &&
           productive < search_limits::feedback_records) {
        size_t chosen = records.size();
        tuple<size_t, size_t, size_t, size_t> best;
        for (size_t rank = 0; rank < records.size(); ++rank) {
            if (visited[rank]) continue;
            const auto &r = trace.records[records[rank]];
            auto key = make_tuple(core_count[r.core], tensor_count[tensor_of[rank]],
                                  region_count[region_of[rank]], rank);
            if (chosen == records.size() || key < best) { chosen = rank; best = key; }
        }
        if (chosen == records.size()) break;
        visited[chosen] = 1; ++scanned;
        const auto index = records[chosen];
        const auto &record = trace.records[index];
        ++core_count[record.core]; ++tensor_count[tensor_of[chosen]];
        ++region_count[region_of[chosen]];
        const int trigger = record.trigger_owner, next = record.next_owner;
        if (trigger < 0 || next < 0 || trigger >= g.size() || next >= g.size() ||
            pos.global[trigger] >= pos.global[next]) { ++stats.rejected_anchor; continue; }
        vector<int> remaining;
        int producer = -1;
        if (record.tensor_index >= 0 && record.tensor_index < int(g.tensors.size())) {
            for (int v : g.tensor_users[record.tensor_index])
                if (seed.core[v] == record.core && pos.global[v] > pos.global[trigger]) remaining.push_back(v);
            stable_sort(remaining.begin(), remaining.end(), [&](int a, int b) {return pos.global[a] < pos.global[b];});
            // Only actual writers can be delayed as producers. A prev_owner may
            // denote a reader or generated COPY; never infer writing from it.
            for (int v : g.tensors[record.tensor_index].producers)
                if (seed.core[v] == record.core && pos.global[v] < pos.global[trigger] &&
                    (producer < 0 || pos.global[v] > pos.global[producer])) producer = v;
        }
        if (remaining.empty()) remaining.push_back(next);
        bool useful = false;
        for (size_t strategy = 0; strategy < buckets.size(); ++strategy) {
            ++stats.strategy_attempts[strategy];
            optional<Solution> proposal;
            if (strategy == 0) proposal = pull_before(g, seed, pos, trigger, {next}, stats);
            else if (strategy == 1) proposal = pull_before(g, seed, pos, trigger, remaining, stats);
            else if (strategy < 4) proposal = push_after(g, seed, pos, trigger, strategy == 2 ? next : remaining.back(), stats);
            else if (producer >= 0) proposal = push_after(g, seed, pos, producer, trigger, stats);
            else { ++stats.rejected_anchor; continue; }
            if (!proposal) continue;
            validate(g, *proposal, cores);
            if (proposal->core != seed.core) throw runtime_error("order feedback changed mapping");
            if (!seen.insert(stream_key(*proposal, cores)).second) { ++stats.rejected_duplicate; continue; }
            proposal->label = "spill_feedback_" + to_string(strategy) + "_" + to_string(index);
            buckets[strategy].push_back(move(*proposal));
            useful = true;
        }
        if (useful) ++productive;
    }
    if (records_examined) *records_examined = scanned;
    stats.records_productive += productive;
    // Interleave strategies, instead of exhausting all slots on pull-next-use.
    for (size_t offset = 0; result.size() < search_limits::feedback_candidates_per_round; ++offset) {
        bool added = false;
        for (size_t strategy = 0; strategy < buckets.size() && result.size() < search_limits::feedback_candidates_per_round; ++strategy)
            if (offset < buckets[strategy].size()) {
                result.push_back(move(buckets[strategy][offset]));
                ++stats.strategy_generated[strategy]; added = true;
            }
        if (!added) break;
    }
    return result;
}

Result improve_spill_order(const Graph &g, int cores, Result incumbent, FeedbackStats *statistics,
                           const vector<Solution> *alternatives, const FeedbackBudget *budget) {
    const size_t replay_limit = budget ? budget->replays : search_limits::feedback_replay_budget;
    const size_t refine_limit = budget ? budget->refinements : search_limits::feedback_refine_budget;
    const size_t stage_refine_limit = budget ? budget->per_stage_refinements : search_limits::feedback_refine_candidates;
    FeedbackStats stats;
    stats.before_cycles = stats.after_cycles = incumbent.refined.cycles;
    auto finish = [&]() {
        incumbent.candidates += stats.traced_candidates;
        incumbent.refined_candidates += stats.refined_candidates;
        if (statistics) *statistics = stats;
        return incumbent;
    };
    if (!search_limits::feedback_enabled) { stats.stop_reason = "disabled"; return finish(); }
    validate(g, incumbent.solution, cores);
    auto original = official_spill_trace(g, incumbent.solution, cores);
    stats.initial_trace_valid = original.valid;
    if (!original.valid) { stats.stop_reason = "initial_trace_invalid"; return finish(); }
    stats.before_spill_bytes = stats.after_spill_bytes = original.spill_bytes;
    stats.route_bytes = stats.after_route_bytes = original.route_bytes;
    const Bytes byte_limit = total(original);
    Bytes best_bytes = byte_limit;
    const double cycle_limit = incumbent.refined.cycles;
    struct Entry { Solution solution; OfficialSpillTrace trace; Metrics proxy; RefinedEstimate refined; bool measured = false; size_t neutral_steps = 0; };
    original.cores.clear();
    Entry start{incumbent.solution, original, incumbent.estimate, incumbent.refined, true};
    set<vector<int>> visited;
    visited.insert(stream_key(incumbent.solution, cores));

    auto replay = [&](Solution candidate, const Entry *parent) -> optional<Entry> {
        if (!visited.insert(stream_key(candidate, cores)).second) { ++stats.rejected_duplicate; return nullopt; }
        validate(g, candidate, cores);
        auto trace = official_spill_trace(g, candidate, cores);
        ++stats.traced_candidates;
        if (!trace.valid) { ++stats.rejected_trace; return nullopt; }
        if (total(trace) > byte_limit || (parent && candidate.core == parent->solution.core &&
            (trace.route_bytes != parent->trace.route_bytes || trace.spill_bytes > parent->trace.spill_bytes))) {
            ++stats.rejected_traffic; return nullopt;
        }
        trace.cores.clear();
        auto proxy = evaluate_proxy(g, cores, Config{}, candidate, 0.03, nullptr);
        const size_t neutral_steps = parent ? parent->neutral_steps + (total(trace) == total(parent->trace)) : 0;
        return Entry{move(candidate), move(trace), move(proxy), {}, false, neutral_steps};
    };
    // Alternate time and bytes rankings. Non-dominated points get first access;
    // exact Step2 bytes and proxy time form the cheap frontier, refined time
    // selects outputs. No claim is made that proxy dominance is exact Step3.
    auto ranking = [&](const vector<Entry> &pool) {
        vector<size_t> by_time(pool.size()), by_bytes(pool.size());
        iota(by_time.begin(), by_time.end(), size_t(0)); by_bytes = by_time;
        auto time = [&](size_t i) {return pool[i].measured ? pool[i].refined.cycles : pool[i].proxy.predicted;};
        stable_sort(by_time.begin(), by_time.end(), [&](size_t a,size_t b) {
            return make_tuple(time(a),total(pool[a].trace),a)<make_tuple(time(b),total(pool[b].trace),b);
        });
        stable_sort(by_bytes.begin(), by_bytes.end(), [&](size_t a,size_t b) {
            return make_tuple(total(pool[a].trace),time(a),a)<make_tuple(total(pool[b].trace),time(b),b);
        });
        vector<size_t> order;
        vector<unsigned char> used(pool.size());
        for (int front = 1; front >= 0; --front)
            for (size_t rank = 0; rank < pool.size(); ++rank)
                for (size_t i : {by_time[rank], by_bytes[rank]}) {
                    if (used[i]) continue;
                    bool dominated = false;
                    if (front) for (size_t j = 0; j < pool.size(); ++j)
                        if (time(j) <= time(i) && total(pool[j].trace) <= total(pool[i].trace) &&
                            (time(j) < time(i) || total(pool[j].trace) < total(pool[i].trace))) { dominated=true; break; }
                    if (dominated) continue;
                    used[i]=1; order.push_back(i);
                }
        return order;
    };
    auto assess = [&](vector<Entry> &pool) {
        size_t stage = 0;
        bool improved = false;
        for (size_t i : ranking(pool)) {
            auto &entry = pool[i];
            if (entry.measured) continue;
            if (stage >= stage_refine_limit ||
                stats.refined_candidates >= refine_limit) break;
            entry.refined = refine_estimate(g, entry.solution, cores); entry.measured = true;
            ++stage; ++stats.refined_candidates;
            if (!entry.refined.valid) { ++stats.rejected_refine; continue; }
            if (entry.refined.cycles > cycle_limit) { ++stats.rejected_cycles; continue; }
            if (make_pair(entry.refined.cycles,total(entry.trace)) < make_pair(incumbent.refined.cycles,best_bytes)) {
                incumbent.solution = entry.solution; incumbent.refined = entry.refined; incumbent.estimate = entry.proxy;
                best_bytes = total(entry.trace);
                stats.after_spill_bytes = entry.trace.spill_bytes; stats.after_route_bytes = entry.trace.route_bytes;
                stats.after_cycles = entry.refined.cycles; stats.best_neutral_steps = entry.neutral_steps; improved = true;
            }
        }
        if (improved) ++stats.accepted_rounds;
    };
    auto beam = [&](vector<Entry> &pool) {
        vector<Entry> chosen;
        for (size_t i : ranking(pool)) {
            if (chosen.size() >= search_limits::feedback_beam) break;
            // Invalid timing models may still yield legal exact Step2 states,
            // but do not spend the next depth on known-infeasible models.
            if (pool[i].measured && !pool[i].refined.valid) continue;
            if (total(pool[i].trace) == byte_limit) ++stats.neutral_retained;
            chosen.push_back(move(pool[i]));
        }
        return chosen;
    };

    vector<Entry> initial;
    if (alternatives) {
        size_t count = 0;
        for (const auto &candidate : *alternatives) {
            if (count++ >= search_limits::feedback_seed_limit || stats.traced_candidates >= replay_limit) break;
            const size_t before = stats.traced_candidates;
            auto entry = replay(candidate,nullptr);
            stats.seeds_traced += stats.traced_candidates - before;
            if (entry) initial.push_back(move(*entry));
        }
    }
    auto mappings = spill_mapping_candidates(g, cores, start.solution, original);
    stats.mapping_generated = mappings.size();
    for (auto &candidate : mappings) {
        if (stats.traced_candidates >= replay_limit) break;
        auto entry = replay(move(candidate),nullptr);
        if (entry) initial.push_back(move(*entry));
    }
    assess(initial);
    // Always search the original winner; fill remaining slots with diverse seeds.
    auto frontier = beam(initial);
    if (frontier.size() >= search_limits::feedback_beam && !frontier.empty()) frontier.pop_back();
    frontier.insert(frontier.begin(),move(start));
    stats.stop_reason = "depth_limit";
    for (size_t depth = 0; depth < search_limits::feedback_rounds; ++depth) {
        if (frontier.empty()) { stats.stop_reason="no_next_state"; break; }
        if (stats.traced_candidates >= replay_limit) { stats.stop_reason="replay_budget"; break; }
        ++stats.rounds;
        vector<vector<Solution>> proposals;
        for (const auto &parent : frontier) {
            size_t examined = 0;
            auto candidates = spill_feedback_candidates(g, cores, parent.solution, parent.trace, &examined, &stats);
            stats.records_examined += examined; stats.generated_candidates += candidates.size();
            proposals.push_back(move(candidates));
        }
        vector<Entry> next;
        size_t replays_before = stats.traced_candidates;
        for (size_t offset=0;offset<search_limits::feedback_candidates_per_round;++offset) {
            for (size_t parent=0;parent<frontier.size();++parent) {
                if (stats.traced_candidates >= replay_limit ||
                    stats.traced_candidates-replays_before >= search_limits::feedback_replays_per_depth) break;
                if (offset >= proposals[parent].size()) continue;
                auto entry = replay(move(proposals[parent][offset]), &frontier[parent]);
                if (entry) next.push_back(move(*entry));
            }
            if (stats.traced_candidates >= replay_limit ||
                stats.traced_candidates-replays_before >= search_limits::feedback_replays_per_depth) break;
        }
        if (next.empty()) { stats.stop_reason="no_eligible_reorder"; break; }
        assess(next);
        frontier = beam(next);
    }
    validate(g, incumbent.solution, cores);
    return finish();
}

string feedback_stats_json(const FeedbackStats &s) {
    ostringstream out;
    out << setprecision(17) << "{";
    bool first=true;
    auto value=[&](const char *name, auto v) { if(!first)out<<',';first=false;out<<'"'<<name<<"\":"<<v; };
    value("before_spill_bytes",s.before_spill_bytes);value("after_spill_bytes",s.after_spill_bytes);
    value("before_route_bytes",s.route_bytes);value("after_route_bytes",s.after_route_bytes);
    auto cycles=[&](const char *name,double v){if(isfinite(v))value(name,v);else value(name,"null");};
    cycles("before_cycles",s.before_cycles);cycles("after_cycles",s.after_cycles);
    value("rounds",s.rounds);value("records_examined",s.records_examined);value("records_productive",s.records_productive);
    value("generated_candidates",s.generated_candidates);value("traced_candidates",s.traced_candidates);
    value("refined_candidates",s.refined_candidates);value("accepted_stages",s.accepted_rounds);
    value("seeds_traced",s.seeds_traced);value("mapping_generated",s.mapping_generated);value("neutral_retained",s.neutral_retained);
    value("best_neutral_steps",s.best_neutral_steps);
    value("rejected_anchor",s.rejected_anchor);value("rejected_dependency",s.rejected_dependency);
    value("rejected_distance",s.rejected_distance);value("rejected_closure",s.rejected_closure);
    value("rejected_duplicate",s.rejected_duplicate);value("rejected_trace",s.rejected_trace);
    value("rejected_traffic",s.rejected_traffic);value("rejected_refine",s.rejected_refine);value("rejected_cycles",s.rejected_cycles);
    value("initial_trace_valid",s.initial_trace_valid?"true":"false");
    out<<",\"strategy_attempts\":[";
    for(size_t i=0;i<5;++i){if(i)out<<',';out<<s.strategy_attempts[i];}
    out<<"],\"strategy_generated\":[";
    for(size_t i=0;i<5;++i){if(i)out<<',';out<<s.strategy_generated[i];}
    out<<"],\"stop_reason\":\""<<s.stop_reason<<"\"}";
    return out.str();
}
} // namespace dag_chain
