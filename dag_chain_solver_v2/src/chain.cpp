#include "chain.hpp"

namespace dag_chain {
namespace {
// Count bytes on real producer-consumer edges, once per tensor/edge.
map<pair<int, int>, double> edge_traffic(const Graph &g) {
    map<pair<int, int>, double> bytes;
    for (const auto &t : g.tensors)
        for (int p : t.producers)
            for (int v : t.consumers)
                if (p != v)
                    bytes[{p, v}] += double(t.size);
    for (const auto &e : g.direct)
        bytes[{e.a, e.b}] += double(e.size);
    return bytes;
}
struct Pins {
    int tensor;
    bool producer, consumer;
};

// Bundles are placement preferences, not contracted DAG nodes. A bundle quotient
// may contain cycles even when the original graph does not; we never schedule it.
vector<int> pack(const Graph &g, const Decomposition &d, int k, double communication) {
    const Config cfg;
    const int count = int(d.bundles.size());
    vector<Row> work(count), loads(k);
    Row total{};
    vector<double> critical(count);
    vector<vector<Pins>> pins(count);
    for (int b = 0; b < count; ++b) {
        map<int, pair<bool, bool>> touched;
        for (int v : d.bundles[b]) {
            work[b][g.ops[v].pipe] += g.weight(v);
            total[g.ops[v].pipe] += g.weight(v);
            critical[b] = max(critical[b], g.rank[v]);
            for (int t : g.ops[v].inputs)
                touched[t].second = true;
            for (int t : g.ops[v].outputs)
                touched[t].first = true;
        }
        for (auto [t, flags] : touched)
            pins[b].push_back({t, flags.first, flags.second});
    }
    vector<int> ordering(count);
    iota(ordering.begin(), ordering.end(), 0);
    auto priority = [&](int b) {
        double load = 0;
        for (int p = 0; p < 5; ++p)
            load = max(load, work[b][p] / max(1.0, total[p] / k));
        return make_tuple(-load, -critical[b], b);
    };
    sort(ordering.begin(), ordering.end(), [&](int a, int b) { return priority(a) < priority(b); });
    vector<unsigned> producer(g.tensors.size()), consumer(g.tensors.size());
    auto pop = [](unsigned x) {
        int n = 0;
        while (x) {
            n += int(x & 1U);
            x >>= 1;
        }
        return n;
    };
    auto tensor_cost = [&](int t, unsigned p, unsigned c) {
        const auto &tensor = g.tensors[t];
        int np = pop(p), nc = pop(c), pairs = np * nc - pop(p & c);
        double copies = tensor.producers.empty() ? nc : 2.0 * pairs;
        if (tensor.final_output)
            copies += np;
        return copies * double(tensor.size) / cfg.bandwidth + pairs * cfg.delay;
    };
    vector<int> assignment(g.size(), -1);
    for (int b : ordering) {
        int best = 0;
        tuple<double, double, int> best_key{INF, INF, 0};
        for (int c = 0; c < k; ++c) {
            double balance = 0, traffic = 0, peak = 0;
            for (int p = 0; p < 5; ++p) {
                double w = work[b][p];
                balance += (2 * loads[c][p] * w + w * w) / max(1.0, total[p] / k);
                peak = max(peak, loads[c][p] + w);
            }
            for (const auto &pin : pins[b]) {
                unsigned oldp = producer[pin.tensor], oldc = consumer[pin.tensor];
                traffic += tensor_cost(pin.tensor, oldp | (pin.producer ? (1U << c) : 0U),
                                       oldc | (pin.consumer ? (1U << c) : 0U)) -
                           tensor_cost(pin.tensor, oldp, oldc);
            }
            auto key = make_tuple(balance + communication * traffic, peak, c);
            if (key < best_key) {
                best_key = key;
                best = c;
            }
        }
        for (int v : d.bundles[b])
            assignment[v] = best;
        for (int p = 0; p < 5; ++p)
            loads[best][p] += work[b][p];
        for (const auto &pin : pins[b]) {
            if (pin.producer)
                producer[pin.tensor] |= 1U << best;
            if (pin.consumer)
                consumer[pin.tensor] |= 1U << best;
        }
    }
    return assignment;
}
struct Entry {
    Solution solution;
    Metrics metrics;
};
struct NoCandidate : runtime_error {
    NoCandidate() : runtime_error("no capacity-feasible candidate in the internal model") {}
};
auto merit(const Entry &e) {
    // This is an internal heuristic, never an official score.
    return make_tuple(e.metrics.score, e.metrics.route + e.metrics.spill, e.solution.core,
                      e.solution.order);
}
} // namespace

Decomposition decompose(const Graph &g, int k, double scale, double communication_weight) {
    Decomposition d;
    const int n = g.size();
    d.parent.assign(n, -1);
    d.next.assign(n, -1);
    const auto bytes = edge_traffic(g);
    auto transfer = [&](int a, int b) {
        auto it = bytes.find({a, b});
        return 1000.0 + (it == bytes.end() ? 0 : 2 * it->second / 60.0);
    };
    vector<double> finish(n);
    for (int v : g.topo) {
        tuple<double, double, int> best{-1, -1, 0};
        for (int u : g.ops[v].pred) {
            auto key =
                make_tuple(finish[u] + communication_weight * transfer(u, v), transfer(u, v), -u);
            if (key > best) {
                best = key;
                d.parent[v] = u;
            }
        }
        finish[v] = g.weight(v) + (d.parent[v] < 0 ? 0 : finish[d.parent[v]]);
    }
    // Resolve fan-out: each predecessor is allowed one continuation, selected by
    // remaining critical work and communication saved. Other edges remain intact.
    for (int v : g.topo)
        if (d.parent[v] >= 0) {
            int u = d.parent[v], old = d.next[u];
            auto priority = [&](int w) {
                return make_pair(g.rank[w] + communication_weight * transfer(u, w), -w);
            };
            if (old < 0 || priority(v) > priority(old))
                d.next[u] = v;
        }
    vector<int> incoming(n, -1);
    for (int u = 0; u < n; ++u)
        if (d.next[u] >= 0)
            incoming[d.next[u]] = u;
    Row total{};
    for (int v : g.topo)
        total[g.ops[v].pipe] += g.weight(v);
    double target = max(1.0, *max_element(total.begin(), total.end()) / k * scale);
    DSU bundles(n);
    for (int root : g.topo)
        if (incoming[root] < 0) {
            vector<int> path;
            Row load{};
            int previous = -1;
            for (int v = root; v >= 0; v = d.next[v]) {
                path.push_back(v);
                int pipe = g.ops[v].pipe;
                if (previous >= 0 && load[pipe] + g.weight(v) <= target)
                    bundles.join(previous, v);
                else
                    load = Row{};
                load[pipe] += g.weight(v);
                previous = v;
            }
            d.paths.push_back(move(path));
        }
    // Conservative handling of partial writes: co-locate all writers, even when
    // that joins different path segments. This is our policy, not an extra rule.
    for (int v = 0; v < n; ++v)
        bundles.join(v, g.writer_group[v]);
    map<int, int> index;
    d.bundle_of.resize(n);
    for (int v : g.topo) {
        int root = bundles.get(v);
        if (!index.count(root)) {
            index[root] = int(d.bundles.size());
            d.bundles.push_back({});
        }
        int b = index[root];
        d.bundle_of[v] = b;
        d.bundles[b].push_back(v);
    }
    return d;
}

Solution repair_writer_order(const Graph &g, const Solution &s,
                             const vector<int> &partial_write_victims) {
    // Give all writers of a threatened tensor the first writer's priority.
    // Propagate that priority to their prerequisites, then topologically sort.
    // This changes no dependency and no core assignment; it only reduces the
    // unrelated work interleaved while a tensor is partially written.
    vector<int> position(g.size()), priority(g.size());
    vector<bool> promoted_writer(g.size());
    for (int i = 0; i < g.size(); ++i)
        position[s.order[i]] = priority[s.order[i]] = i;
    for (int t : partial_write_victims) {
        int first = g.size();
        for (int v : g.tensors[t].producers)
            first = min(first, position[v]);
        for (int v : g.tensors[t].producers) {
            priority[v] = min(priority[v], first);
            promoted_writer[v] = true;
        }
    }
    for (auto it = g.topo.rbegin(); it != g.topo.rend(); ++it)
        for (int u : g.ops[*it].pred)
            priority[u] = min(priority[u], priority[*it]);
    // Prepare a later writer's inputs before opening the partial output, when
    // dependencies permit. This also handles writers with different ancestors.
    auto key = [&](int v) { return make_tuple(priority[v], bool(promoted_writer[v]), position[v], v); };
    auto cmp = [&](int a, int b) { return key(a) > key(b); };
    priority_queue<int, vector<int>, decltype(cmp)> ready(cmp);
    vector<int> degree(g.size());
    for (int v = 0; v < g.size(); ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v])
            ready.push(v);
    }
    Solution result = s;
    result.order.clear();
    while (!ready.empty()) {
        int v = ready.top();
        ready.pop();
        result.order.push_back(v);
        for (int w : g.ops[v].succ)
            if (!--degree[w])
                ready.push(w);
    }
    return result;
}

static Result solve_at_core_count(const Graph &g, int k, bool baseline_only) {
    if (k < 2 || k > 5)
        throw runtime_error("core count must be 2..5");
    const Config cfg;
    vector<Entry> pool;
    set<pair<vector<int>, vector<int>>> seen;
    size_t evaluated = 0, repair_attempts = 0;
    auto assess = [&](const Solution &s, Metrics &m) {
        validate(g, s, k);
        if (!seen.insert({s.core, s.order}).second)
            return false;
        m = evaluate_proxy(g, k, cfg, s, 0.03, nullptr);
        ++evaluated;
        if (m.memory_warning)
            return true;
        pool.push_back({s, m});
        stable_sort(pool.begin(), pool.end(),
                    [](const Entry &a, const Entry &b) { return merit(a) < merit(b); });
        if (pool.size() > 4)
            pool.resize(4);
        return true;
    };
    auto offer = [&](Solution s) {
        s.core = g.normalize_writers(move(s.core));
        Metrics m;
        if (!assess(s, m) || !m.memory_warning || baseline_only ||
            m.partial_write_victims.empty())
            return;
        const string label = s.label;
        // Two targeted repair passes, followed by one dependency-DFS fallback.
        // A repaired candidate must pass exactly the same guard as any other.
        for (int pass = 0; pass < 2 && !m.partial_write_victims.empty(); ++pass) {
            auto repaired = repair_writer_order(g, s, m.partial_write_victims);
            if (repaired.order == s.order)
                break;
            repaired.label = label + "_writer_repair_" + to_string(pass);
            ++repair_attempts;
            Metrics next;
            if (!assess(repaired, next))
                break;
            s = move(repaired);
            m = move(next);
            if (!m.memory_warning)
                break;
        }
        if (s.order != g.dfs_order) {
            s.order = g.dfs_order;
            s.label = label + "_writer_dfs";
            ++repair_attempts;
            Metrics next;
            assess(s, next);
        }
    };
    // Identical singleton baselines are available to both algorithms. A chain
    // proposal must beat them under the same cost model to be selected.
    for (auto [mode, balance, window] : vector<tuple<string, double, int>>{
             {"critical", 0, 1}, {"critical", 0.5, 8}, {"dfs", 0.3, 4}}) {
        SeedOptions opt;
        opt.mode = mode;
        opt.balance = balance;
        opt.window = window;
        auto s = operation_plan(g, k, cfg, opt, nullptr, nullptr);
        s.label = "baseline_" + mode;
        offer(move(s));
    }
    Solution single;
    single.core.assign(g.size(), 0);
    single.order = g.dfs_order;
    single.label = "single_core_fallback";
    offer(move(single));
    Result result;
    // Fixed, input-independent parameter grid; no wall-clock stopping or RNG.
    // Bound variant count from graph work alone for large unseen inputs.
    size_t graph_work = size_t(g.size()) + g.edge_count + g.tensors.size();
    int variant_limit = graph_work > 200000 ? 2 : graph_work > 80000 ? 4 : 6;
    vector<Decomposition> decompositions;
    if (!baseline_only) {
        const vector<tuple<double, double, double>> variants = {
            {0.5, 0, 0.5},    {1.0, 0, 1.0},    {0.25, 0, 0.25},
            {0.5, 0.25, 1.0}, {1.0, 0.25, 2.0}, {0.25, 0.25, 0.5}};
        for (int i = 0; i < variant_limit; ++i) {
            auto [scale, critical_comm, pack_comm] = variants[i];
            auto d = decompose(g, k, scale, critical_comm);
            if (i == 0) {
                result.paths = d.paths.size();
                result.bundles = d.bundles.size();
            }
            auto a = pack(g, d, k, pack_comm);
            for (const string &mode : {string("critical"), string("dfs")}) {
                SeedOptions opt;
                opt.mode = mode;
                opt.window = 8;
                opt.memory = 0.5;
                auto s = operation_plan(g, k, cfg, opt, &a, nullptr);
                s.label = "chain_" + to_string(i) + "_" + mode;
                offer(move(s));
            }
            decompositions.push_back(move(d));
        }
    }
    // Search a bounded number of whole-segment migrations. The incremental
    // traffic ledger cheaply screens moves; only a few get a full proxy replay.
    if (!baseline_only && !pool.empty() && graph_work <= 200000) {
        for (int round = 0; round < 2; ++round) {
            Solution base = pool.front().solution;
            const auto &d = decompositions[round % decompositions.size()];
            vector<int> order(d.bundles.size());
            iota(order.begin(), order.end(), 0);
            vector<double> importance(order.size());
            for (size_t b = 0; b < d.bundles.size(); ++b)
                for (int v : d.bundles[b])
                    importance[b] += g.weight(v);
            sort(order.begin(), order.end(), [&](int a, int b) {
                return make_pair(-importance[a], a) < make_pair(-importance[b], b);
            });
            if (order.size() > 24)
                order.resize(24);
            TrafficState ledger(g, cfg, k, base.core);
            vector<tuple<double, int, int>> moves;
            for (int b : order)
                for (int c = 0; c < k; ++c) {
                    vector<pair<int, int>> undo;
                    for (int v : d.bundles[b])
                        if (ledger.core[v] != c) {
                            undo.push_back({v, ledger.core[v]});
                            ledger.move_op(v, c);
                        }
                    if (!undo.empty())
                        moves.push_back({ledger.light_score(0.03), b, c});
                    for (auto it = undo.rbegin(); it != undo.rend(); ++it)
                        ledger.move_op(it->first, it->second);
                }
            sort(moves.begin(), moves.end());
            for (size_t i = 0; i < min<size_t>(4, moves.size()); ++i) {
                auto [cost, b, c] = moves[i];
                (void)cost;
                auto a = base.core;
                for (int v : d.bundles[b])
                    a[v] = c;
                SeedOptions opt;
                opt.mode = round ? "dfs" : "critical";
                opt.window = 8;
                opt.memory = 0.5;
                auto s = operation_plan(g, k, cfg, opt, &a, nullptr);
                s.label = "chain_migration_" + to_string(round) + "_" + to_string(i);
                offer(move(s));
            }
        }
    }
    if (pool.empty())
        throw NoCandidate();
    result.solution = pool.front().solution;
    result.estimate = pool.front().metrics;
    result.candidates = evaluated;
    result.repair_attempts = repair_attempts;
    result.generated_for_cores = k;
    return result;
}

bool prefer_smaller_core_plan(const Metrics &smaller, const Metrics &current) {
    const Config cfg;
    // The original scalar proxy can trade away too much computation parallelism
    // to reduce spill bytes. For fewer cores, require agreement between two
    // timing sketches, not just a lower scalar score. These are heuristic
    // estimates, not certified bounds on the external simulator.
    double conservative = max(smaller.sketch + smaller.spill / cfg.bandwidth,
                              (smaller.route + smaller.spill) / cfg.bandwidth);
    double optimistic = max(current.sketch, (current.route + current.spill) / cfg.bandwidth);
    return smaller.score < current.score && conservative <= optimistic;
}

Result solve(const Graph &g, int k, bool baseline_only) {
    if (k < 2 || k > 5)
        throw runtime_error("core count must be 2..5");
    if (baseline_only)
        return solve_at_core_count(g, k, true);
    // N is the available number of cores, not a requirement to occupy them all.
    // Generate each smaller-core portfolio once; the final JSON pads unused cores
    // with empty schedules. No recursive solve calls or wall-clock decisions.
    vector<Result> portfolios;
    size_t evaluated = 0, repairs = 0;
    for (int active = 2; active <= k; ++active) {
        try {
            auto candidate = solve_at_core_count(g, active, false);
            evaluated += candidate.candidates;
            repairs += candidate.repair_attempts;
            portfolios.push_back(move(candidate));
        } catch (const NoCandidate &) {
            // A smaller placement may fail while a larger one is feasible.
        }
    }
    if (portfolios.empty())
        throw NoCandidate();
    // Start with the largest feasible budget, then accept clear reductions.
    Result best = move(portfolios.back());
    for (int i = int(portfolios.size()) - 2; i >= 0; --i)
        if (prefer_smaller_core_plan(portfolios[i].estimate, best.estimate))
            best = move(portfolios[i]);
    best.candidates = evaluated;
    best.repair_attempts = repairs;
    validate(g, best.solution, k);
    return best;
}
} // namespace dag_chain
