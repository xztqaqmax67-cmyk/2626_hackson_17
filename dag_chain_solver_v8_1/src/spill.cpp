#include "spill.hpp"
#include "search_limits.hpp"
#include "solution_key.hpp"

namespace dag_chain {
namespace {
struct OrderStyle {
    int priority; // 0: critical path; 1: DFS; 2: the incumbent's order.
    double memory, pressure, drain;
};
// Search weights, not machine rules. Larger memory weights may defer ready
// compute to finish a resident tensor's consumers before opening another branch.
// The four styles are deliberately bounded by spill_reorder_variants.
constexpr array<OrderStyle, 4> styles{{
    {0, 4.0, 4.0, 0.5}, {1, 4.0, 8.0, 1.0},
    {2, 8.0, 8.0, 2.0}, {0, 12.0, 12.0, 2.0}
}};

double pressure_price(const Graph &g, const Config &cfg, const MemoryOrder &memory,
                      int v, int core, const OrderStyle &style) {
    array<double, 2> added{}, released{}, progress{};
    for (int t : g.op_tensor_uses[v]) {
        const auto &tensor = g.tensors[t];
        const size_t key = size_t(t) * memory.k + core;
        const int remaining = memory.remaining[key];
        const double bytes = double(tensor.size);
        if (!memory.live[key])
            added[tensor.pos] += bytes;
        if (remaining == 1)
            released[tensor.pos] += bytes;
        else if (memory.live[key] && remaining > 1)
            // Reward progress toward the last use even when the current reader
            // cannot free the tensor yet. New outputs receive no drain reward.
            progress[tensor.pos] += bytes / double(remaining);
    }
    double price = 0;
    for (int m = 0; m < 2; ++m) {
        const double limit = double(cfg.spill_limit(m));
        const double before = memory.used[core][m];
        const double during = before + added[m];
        const double after = max(0.0, during - released[m]);
        // Allocation precedes release: last-use inputs still occupy memory
        // while this operation's outputs are allocated. The quadratic potential
        // distinguishes UB pressure from an equal byte count in larger L1.
        const double potential = (after * after - before * before) / limit;
        price += added[m] - released[m] + 0.5 * potential +
                 style.pressure * max(0.0, during - limit) - style.drain * progress[m];
    }
    return price / cfg.bandwidth;
}

Solution pressure_order(const Graph &g, int cores, const Config &cfg,
                        const Solution &seed, const vector<int> &assignment,
                        const OrderStyle &style, size_t window) {
    const int n = g.size();
    vector<int> original_position(n), degree(n);
    for (int i = 0; i < n; ++i)
        original_position[seed.order[i]] = i;
    vector<pair<double, int>> priority(n);
    for (int v = 0; v < n; ++v) {
        const double first = style.priority == 1 ? double(g.dfs_pos[v]) :
                             style.priority == 2 ? double(original_position[v]) : -g.rank[v];
        priority[v] = {first, original_position[v]};
    }
    auto later = [&](int a, int b) { return make_pair(priority[a], a) > make_pair(priority[b], b); };
    priority_queue<int, vector<int>, decltype(later)> ready(later);
    for (int v = 0; v < n; ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v]) ready.push(v);
    }
    Placement placement(g, cores, cfg);
    MemoryOrder memory(g, cfg, cores, assignment);
    Solution result;
    result.core = assignment;
    result.order.reserve(n);
    vector<int> examined;
    while (!ready.empty()) {
        examined.clear();
        for (size_t j = 0; j < window && !ready.empty(); ++j) {
            examined.push_back(ready.top());
            ready.pop();
        }
        Proposal chosen;
        tuple<double, pair<double, int>, int> best{INF, {INF, 0}, 0};
        for (int v : examined) {
            const int core = assignment[v];
            auto trial = placement.trial(v, core);
            const double score = trial.start + style.memory * pressure_price(g, cfg, memory, v, core, style);
            const auto key = make_tuple(score, priority[v], v);
            if (key < best) {
                best = key;
                chosen = move(trial);
            }
        }
        const int v = chosen.v;
        for (int other : examined)
            if (other != v) ready.push(other);
        placement.commit(chosen);
        memory.commit(v, assignment[v]);
        result.order.push_back(v);
        for (int successor : g.ops[v].succ)
            if (!--degree[successor]) ready.push(successor);
    }
    validate(g, result, cores);
    return result;
}

struct PressureProfile {
    // These are serial logical lifetimes, not actual asynchronous allocation
    // intervals. They rank neighborhoods only; they never certify feasibility.
    vector<double> exposure;
    vector<array<double, 2>> peak;
};

PressureProfile pressure_profile(const Graph &g, int cores, const Config &cfg,
                                  const Solution &seed) {
    const size_t keys = g.tensors.size() * size_t(cores);
    vector<int> first(keys, -1), last(keys, -1), length(cores);
    for (int v : seed.order) {
        const int c = seed.core[v], position = length[c]++;
        for (int t : g.op_tensor_uses[v]) {
            const size_t key = size_t(t) * cores + c;
            if (first[key] < 0) first[key] = position;
            last[key] = position;
        }
    }
    vector<array<vector<double>, 2>> difference(cores), prefix(cores);
    for (int c = 0; c < cores; ++c)
        for (int m = 0; m < 2; ++m) {
            difference[c][m].resize(size_t(length[c]) + 1);
            prefix[c][m].resize(size_t(length[c]) + 1);
        }
    for (size_t t = 0; t < g.tensors.size(); ++t)
        for (int c = 0; c < cores; ++c) {
            const size_t key = t * cores + c;
            if (first[key] < 0) continue;
            const auto &tensor = g.tensors[t];
            difference[c][tensor.pos][first[key]] += double(tensor.size);
            difference[c][tensor.pos][size_t(last[key]) + 1] -= double(tensor.size);
        }
    PressureProfile result;
    result.exposure.resize(keys);
    result.peak.resize(cores);
    for (int c = 0; c < cores; ++c)
        for (int m = 0; m < 2; ++m) {
            double used = 0;
            const double limit = double(cfg.spill_limit(m));
            for (int i = 0; i < length[c]; ++i) {
                used += difference[c][m][i];
                result.peak[c][m] = max(result.peak[c][m], used);
                prefix[c][m][size_t(i) + 1] = prefix[c][m][i] + max(0.0, used / limit - 1.0);
            }
        }
    for (size_t t = 0; t < g.tensors.size(); ++t)
        for (int c = 0; c < cores; ++c) {
            const size_t key = t * cores + c;
            if (first[key] < 0) continue;
            const auto &sum = prefix[c][g.tensors[t].pos];
            result.exposure[key] = double(g.tensors[t].size) *
                                  (sum[size_t(last[key]) + 1] - sum[first[key]]);
        }
    return result;
}
} // namespace

vector<Solution> spill_candidates(const Graph &g, int cores, const Solution &seed) {
    vector<Solution> result;
    if (!search_limits::spill_max_candidates ||
        g.ops.size() + g.edge_count + g.tensors.size() > search_limits::large_graph_work)
        return result;
    validate(g, seed, cores);
    const Config cfg;
    const bool medium = g.ops.size() + g.edge_count + g.tensors.size() > search_limits::medium_graph_work;
    const size_t window = max(size_t(1), medium ? search_limits::spill_ready_window / 2 :
                                               search_limits::spill_ready_window);
    auto append = [&](Solution candidate) {
        if (result.size() >= search_limits::spill_max_candidates ||
            (candidate.core == seed.core && candidate.order == seed.order)) return;
        for (const auto &existing : result)
            if (candidate.core == existing.core && candidate.order == existing.order) return;
        result.push_back(move(candidate));
    };
    for (size_t style = 0; style < min(styles.size(), size_t(search_limits::spill_reorder_variants)) &&
                           result.size() < search_limits::spill_max_candidates; ++style) {
        auto candidate = pressure_order(g, cores, cfg, seed, seed.core, styles[style], window);
        candidate.label = "spill_drain_order_" + to_string(style);
        append(move(candidate));
    }
    if (!search_limits::spill_move_keep || !search_limits::spill_move_evaluations ||
        result.size() >= search_limits::spill_max_candidates) return result;

    const auto profile = pressure_profile(g, cores, cfg, seed);
    vector<pair<double, int>> hotspots;
    for (int t = 0; t < int(g.tensors.size()); ++t) {
        double exposure = 0;
        for (int c = 0; c < cores; ++c) exposure += profile.exposure[size_t(t) * cores + c];
        if (exposure > 0) hotspots.push_back({-exposure, t});
    }
    sort(hotspots.begin(), hotspots.end());
    const size_t hotspot_limit = medium ? search_limits::spill_hotspot_tensors / 2 :
                                         search_limits::spill_hotspot_tensors;
    if (hotspots.size() > hotspot_limit) hotspots.resize(hotspot_limit);
    vector<vector<int>> writers(g.size());
    for (int v = 0; v < g.size(); ++v) writers[g.writer_group[v]].push_back(v);
    struct Move {
        double score;
        int target;
        vector<int> nodes;
    };
    vector<Move> moves;
    set<pair<vector<int>, int>> seen_moves;
    TrafficState ledger(g, cfg, cores, seed.core);
    for (const auto &[negative_exposure, tensor_index] : hotspots) {
        (void)negative_exposure;
        const auto &tensor = g.tensors[tensor_index];
        for (int source = 0; source < cores; ++source) {
            const double exposure = profile.exposure[size_t(tensor_index) * cores + source];
            if (exposure <= 0) continue;
            vector<int> group_ids;
            for (int v : g.tensor_users[tensor_index])
                if (seed.core[v] == source) group_ids.push_back(g.writer_group[v]);
            Graph::unique_vec(group_ids);
            vector<int> nodes;
            for (int group : group_ids) {
                nodes.insert(nodes.end(), writers[group].begin(), writers[group].end());
                if (nodes.size() > search_limits::spill_neighborhood_nodes) break;
            }
            if (nodes.empty() || nodes.size() > search_limits::spill_neighborhood_nodes) continue;
            Graph::unique_vec(nodes);
            for (int target = 0; target < cores; ++target) {
                if (target == source || !seen_moves.insert({nodes, target}).second) continue;
                for (int v : nodes) ledger.move_op(v, target);
                // Do not reject a move just because it adds boundary transfers:
                // it can save more spill/reload traffic. Relief is a deliberately
                // bounded heuristic; all shortlisted moves get a full proxy.
                const double limit = double(cfg.spill_limit(tensor.pos));
                const double relief = 2 * double(tensor.size) * min(4.0, exposure / max(1.0, double(tensor.size)));
                const double destination_pressure = max(0.0, (profile.peak[target][tensor.pos] +
                                                              double(tensor.size)) / limit - 1.0);
                const double score = ledger.light_score(0.03) +
                                     (2 * double(tensor.size) * destination_pressure - relief) / cfg.bandwidth;
                moves.push_back({score, target, nodes});
                for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) ledger.move_op(*it, source);
            }
        }
    }
    stable_sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
        return tie(a.score, a.target, a.nodes) < tie(b.score, b.target, b.nodes);
    });
    struct Ranked { Solution solution; Metrics metrics; };
    vector<Ranked> ranked;
    for (size_t i = 0; i < min(moves.size(), search_limits::spill_move_evaluations); ++i) {
        auto assignment = seed.core;
        for (int v : moves[i].nodes) assignment[v] = moves[i].target;
        auto candidate = pressure_order(g, cores, cfg, seed, assignment, styles[2], window);
        candidate.label = "spill_pressure_move_" + to_string(i);
        auto metrics = evaluate_proxy(g, cores, cfg, candidate, 0.03, nullptr);
        if (!metrics.memory_warning) ranked.push_back({move(candidate), move(metrics)});
    }
    stable_sort(ranked.begin(), ranked.end(), [](const Ranked &a, const Ranked &b) {
        return detail::solution_key(a.metrics.score, a.metrics.route + a.metrics.spill, a.solution) <
               detail::solution_key(b.metrics.score, b.metrics.route + b.metrics.spill, b.solution);
    });
    for (size_t i = 0; i < min(ranked.size(), search_limits::spill_move_keep); ++i)
        append(move(ranked[i].solution));
    return result;
}
} // namespace dag_chain

