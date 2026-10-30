#include "timing_feedback.hpp"

namespace dag_chain {
namespace {
vector<int> streams(const Solution &s, int cores) {
    vector<vector<int>> by_core(cores);
    for (int v : s.order) by_core[s.core[v]].push_back(v);
    vector<int> key;
    for (const auto &part : by_core) {
        key.insert(key.end(), part.begin(), part.end()); key.push_back(-1);
    }
    return key;
}
struct Proposal { int anchor = -1, root = -1; double score = 0; };
} // namespace

vector<Solution> timing_feedback_candidates(const Graph &g, int cores,
    const Solution &seed, const OfficialTimingResult &timing, size_t limit,
    const TimingFeedbackLimits &limits) {
    vector<Solution> result;
    validate(g, seed, cores);
    if (!timing.valid || timing.cores.size() != size_t(cores) || !limit ||
        !limits.previous_pipe_ops || !limits.closure_attempts || !limits.closure_nodes ||
        !limits.closure_edges || !limits.per_core_distance || !g.size()) return result;
    const int n = g.size();
    unordered_map<i64, int> original;
    for (int v = 0; v < n; ++v) original.emplace(g.ops[v].id, v);
    vector<i64> start(n), end(n);
    vector<unsigned char> found(n);
    for (int c = 0; c < cores; ++c) {
        if (timing.cores[c].core != c) return result;
        for (const auto &op : timing.cores[c].timeline) {
            auto it = original.find(op.id);
            if (it == original.end()) continue; // Generated COPYs are not movable original ops.
            const int v = it->second;
            // Execution IDs are scoped by core. A generated COPY on this core
            // can reuse the numeric ID of an original op assigned elsewhere.
            if (seed.core[v] != c) continue;
            if (found[v] || op.start < 0 || op.end <= op.start) return result;
            start[v] = op.start; end[v] = op.end; found[v] = 1;
        }
    }
    if (find(found.begin(), found.end(), 0) != found.end()) return result;
    vector<int> position(n), local_position(n);
    vector<vector<int>> core_positions(cores);
    vector<array<vector<int>, 5>> pipe_order(cores);
    for (int i = 0; i < n; ++i) {
        const int v = seed.order[i], c = seed.core[v];
        position[v] = i; local_position[v] = int(core_positions[c].size());
        core_positions[c].push_back(i); pipe_order[c][g.ops[v].pipe].push_back(v);
    }
    vector<i64> ready(n);
    vector<double> cross_wait(n);
    for (int v = 0; v < n; ++v) {
        for (int p : g.ops[v].pred) ready[v] = max(ready[v], end[p]);
        for (int w : g.ops[v].succ) if (seed.core[v] != seed.core[w])
            cross_wait[v] = max(cross_wait[v], double(max<i64>(0, start[w] - end[v])));
    }
    // Original-parent completion is only a readiness LOWER BOUND: generated
    // COPYs, cross-core waits and memory reuse are deliberately left to replay.
    // Two priorities cover idle holes and delayed downstream/remote work.
    array<vector<Proposal>, 2> ranked;
    double max_rank = 1;
    for (double rank : g.rank) max_rank = max(max_rank, rank);
    for (int c = 0; c < cores; ++c) for (const auto &pipe : pipe_order[c])
        for (size_t j = 1; j < pipe.size(); ++j) {
            const int v = pipe[j];
            const size_t first = j > limits.previous_pipe_ops ? j - limits.previous_pipe_ops : 0;
            for (size_t a = first; a < j; ++a) {
                const int anchor = pipe[a];
                const i64 prior_end = a ? end[pipe[a - 1]] : 0;
                const i64 lower_bound = max(ready[v], prior_end);
                const double advance = double(max<i64>(0, start[v] - lower_bound));
                const double hole = double(max<i64>(0, start[anchor] - lower_bound));
                if (hole > 0)
                    ranked[0].push_back({anchor, v, min(hole, double(end[v] - start[v])) *
                        (1.0 + g.rank[v] / max_rank)});
                const double downstream = max(0.0, g.rank[v] - g.rank[anchor]);
                if (advance > 0 && (downstream > 0 || cross_wait[v] > 0))
                    ranked[1].push_back({anchor, v, advance *
                        (downstream / max_rank + cross_wait[v] / max(1.0, double(timing.cycles)))});
            }
        }
    for (auto &bucket : ranked) stable_sort(bucket.begin(), bucket.end(), [&](const Proposal &a, const Proposal &b) {
        return make_tuple(-a.score, position[a.root], position[a.anchor], a.root, a.anchor) <
               make_tuple(-b.score, position[b.root], position[b.anchor], b.root, b.anchor);
    });
    set<pair<int, int>> attempted;
    set<vector<int>> seen{streams(seed, cores)};
    vector<size_t> selected(n);
    size_t attempts = 0;
    for (size_t offset = 0; result.size() < limit && attempts < limits.closure_attempts; ++offset) {
        bool any = false;
        for (size_t direction = 0; direction < ranked.size() && result.size() < limit &&
             attempts < limits.closure_attempts; ++direction) {
            if (offset >= ranked[direction].size()) continue;
            any = true;
            const auto &proposal = ranked[direction][offset];
            if (!attempted.insert({proposal.anchor, proposal.root}).second) continue;
            ++attempts;
            const int boundary = position[proposal.anchor];
            vector<int> boundary_local(cores);
            for (int c = 0; c < cores; ++c)
                boundary_local[c] = int(lower_bound(core_positions[c].begin(), core_positions[c].end(), boundary) -
                                        core_positions[c].begin());
            vector<int> stack{proposal.root}; size_t count = 0, edges = 0;
            bool legal = true;
            while (!stack.empty() && legal) {
                const int v = stack.back(); stack.pop_back();
                if (position[v] < boundary || selected[v] == attempts) continue;
                if (v == proposal.anchor || size_t(local_position[v] - boundary_local[seed.core[v]]) >
                    limits.per_core_distance || ++count > limits.closure_nodes) { legal = false; break; }
                selected[v] = attempts;
                // Closure includes predecessors on EVERY core. It never treats
                // writer/co-location groups as atomic execution blocks.
                for (int p : g.ops[v].pred) {
                    if (++edges > limits.closure_edges) { legal = false; break; }
                    if (position[p] >= boundary) stack.push_back(p);
                }
            }
            if (!legal || !count) continue;
            Solution candidate = seed;
            candidate.order.clear();
            for (int v : seed.order) {
                if (v == proposal.anchor)
                    for (int moved : seed.order) if (selected[moved] == attempts) candidate.order.push_back(moved);
                if (selected[v] != attempts) candidate.order.push_back(v);
            }
            if (!seen.insert(streams(candidate, cores)).second) continue;
            candidate.label = "timing_feedback_" + to_string(direction) + "_" +
                to_string(g.ops[proposal.root].id) + "_before_" + to_string(g.ops[proposal.anchor].id);
            validate(g, candidate, cores);
            result.push_back(move(candidate));
        }
        if (!any) break;
    }
    return result;
}
} // namespace dag_chain
