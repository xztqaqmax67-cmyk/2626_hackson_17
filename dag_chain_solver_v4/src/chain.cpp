#include "chain.hpp"
#include "parallel_jobs.hpp"
#include <optional>

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

namespace {
struct Analysis {
    map<pair<int, int>, double> bytes;
    vector<double> finish;
    explicit Analysis(const Graph &g) : bytes(edge_traffic(g)), finish(g.size()) {
        for (int v : g.topo) {
            double latest = 0;
            for (int u : g.ops[v].pred)
                latest = max(latest, finish[u]);
            finish[v] = latest + g.weight(v);
        }
    }
};
Decomposition make_paths(const Graph &g, const Analysis &analysis, double communication_weight,
                         bool recover) {
    Decomposition d;
    const int n = g.size();
    d.parent.assign(n, -1);
    d.next.assign(n, -1);
    const auto &bytes = analysis.bytes;
    auto transfer = [&](int a, int b) {
        auto it = bytes.find({a, b});
        return 1000.0 + (it == bytes.end() ? 0 : 2 * it->second / 60.0);
    };
    const auto &finish = analysis.finish;
    d.earliest_finish = finish;
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
    // A child that loses its preferred predecessor may use another free one.
    // This produces a maximal path cover, not a claimed minimum path cover.
    if (recover)
        for (int v : g.topo)
            if (incoming[v] < 0) {
                int best = -1;
                tuple<double, double, int> best_key{-1, -1, 0};
                for (int u : g.ops[v].pred)
                    if (d.next[u] < 0) {
                        auto key = make_tuple(finish[u] + communication_weight * transfer(u, v),
                                              transfer(u, v), -u);
                        if (key > best_key) {
                            best_key = key;
                            best = u;
                        }
                    }
                if (best >= 0) {
                    d.next[best] = v;
                    incoming[v] = best;
                }
            }
    for (int root : g.topo)
        if (incoming[root] < 0) {
            vector<int> path;
            for (int v = root; v >= 0; v = d.next[v])
                path.push_back(v);
            d.paths.push_back(move(path));
        }
    return d;
}
Decomposition segment(const Graph &g, const Decomposition &paths, int k, double scale,
                      bool memory_cuts) {
    Decomposition d = paths;
    const int n = g.size();
    Row total{};
    for (int v : g.topo)
        total[g.ops[v].pipe] += g.weight(v);
    double target = max(1.0, *max_element(total.begin(), total.end()) / k * scale);
    DSU bundles(n);
    const Config cfg;
    for (const auto &path : d.paths) {
            Row load{};
            int previous = -1;
            set<int> footprint;
            array<double, 2> bytes{};
            for (int v : path) {
                vector<int> uses = g.ops[v].inputs;
                uses.insert(uses.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
                Graph::unique_vec(uses);
                auto projected = bytes;
                for (int t : uses)
                    if (!footprint.count(t))
                        projected[g.tensors[t].pos] += double(g.tensors[t].size);
                bool pressure = memory_cuts &&
                    (projected[0] > double(cfg.spill_limit(0)) ||
                     projected[1] > double(cfg.spill_limit(1)));
                int pipe = g.ops[v].pipe;
                if (previous >= 0 && load[pipe] + g.weight(v) <= target && !pressure)
                    bundles.join(previous, v);
                else {
                    load = Row{};
                    footprint.clear();
                    bytes = {};
                }
                for (int t : uses)
                    if (footprint.insert(t).second)
                        bytes[g.tensors[t].pos] += double(g.tensors[t].size);
                load[pipe] += g.weight(v);
                previous = v;
            }
    }
    // The updated official plan contract requires all writers to share a core,
    // even when binding them joins different path segments.
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
} // namespace

Decomposition decompose(const Graph &g, int k, double scale, double communication_weight) {
    Analysis analysis(g);
    return segment(g, make_paths(g, analysis, communication_weight, true), k, scale, false);
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

static Result solve_at_core_count(const Graph &g, int k, bool baseline_only,
                                  const vector<Decomposition> &path_covers) {
    if (k < 2 || k > 5)
        throw runtime_error("core count must be 2..5");
    const Config cfg;
    vector<Entry> pool;
    vector<Entry> legacy_pool, retained_seeds;
    bool collecting_legacy = true;
    set<pair<vector<int>, vector<int>>> seen;
    size_t evaluated = 0, repair_attempts = 0;
    auto prune = [&]() {
        if (pool.size() <= 24)
            return;
        vector<int> order(pool.size());
        iota(order.begin(), order.end(), 0);
        set<int> keep;
        for (int mode = 0; mode < 4; ++mode) {
            auto key = [&](int i) {
                const auto &m = pool[i].metrics;
                double cost = mode == 0 ? m.score : mode == 1 ? m.route + m.spill :
                              mode == 2 ? m.sketch : m.score + m.spill / cfg.bandwidth;
                return make_tuple(cost, m.score, i);
            };
            stable_sort(order.begin(), order.end(), [&](int a, int b) { return key(a) < key(b); });
            int limit = mode == 0 ? 10 : mode == 1 ? 6 : 4;
            for (int i = 0; i < min(limit, int(order.size())); ++i)
                keep.insert(order[i]);
        }
        vector<Entry> retained;
        for (int i : keep)
            retained.push_back(move(pool[i]));
        pool = move(retained);
    };
    auto assess = [&](const Solution &s, Metrics &m) {
        validate(g, s, k);
        vector<int> streams;
        for (int c = 0; c < k; ++c) {
            for (int v : s.order)
                if (s.core[v] == c)
                    streams.push_back(v);
            streams.push_back(-1);
        }
        // Equivalent global interleavings describe the same submitted plan.
        if (!seen.insert({s.core, streams}).second)
            return false;
        m = evaluate_proxy(g, k, cfg, s, 0.03, nullptr);
        ++evaluated;
        if (m.memory_warning)
            return true;
        if (collecting_legacy) {
            legacy_pool.push_back({s, m});
            auto legacy_key = [&](const Entry &e) {
                return make_tuple(e.metrics.score + e.metrics.spill / cfg.bandwidth,
                                  e.metrics.route + e.metrics.spill, e.solution.core,
                                  e.solution.order);
            };
            stable_sort(legacy_pool.begin(), legacy_pool.end(), [&](const Entry &a, const Entry &b) {
                return legacy_key(a) < legacy_key(b);
            });
            if (legacy_pool.size() > 4) legacy_pool.resize(4);
        }
        pool.push_back({s, m});
        prune();
        stable_sort(pool.begin(), pool.end(),
                    [](const Entry &a, const Entry &b) { return merit(a) < merit(b); });
        return true;
    };
    auto offer = [&](Solution s) {
        s.core = g.normalize_writers(move(s.core));
        Metrics m;
        if (!assess(s, m) || baseline_only || m.partial_write_victims.empty())
            return;
        const string label = s.label;
        // Partial-write spilling is supported by the updated Step 2. Keep the
        // original candidate and try writer clustering only to reduce traffic.
        // Two targeted passes and one DFS alternative keep the work bounded.
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
            if (m.partial_write_victims.empty())
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
    int variant_limit = graph_work > 200000 ? 2 : graph_work > 80000 ? 6 : 10;
    vector<Decomposition> decompositions;
    auto retain_legacy_search = [&]() {
        // Preserve the earlier search trajectory as additional proposals. It
        // gets the same final event evaluation, with no preference for version.
        if (graph_work <= 200000 && !decompositions.empty())
            for (int round = 0; round < 2 && !legacy_pool.empty(); ++round) {
                Solution base = legacy_pool.front().solution;
                const auto &d = decompositions[round % decompositions.size()];
                vector<int> order(d.bundles.size());
                iota(order.begin(), order.end(), 0);
                vector<double> work(order.size());
                for (size_t b = 0; b < d.bundles.size(); ++b)
                    for (int v : d.bundles[b]) work[b] += g.weight(v);
                stable_sort(order.begin(), order.end(), [&](int a, int b) {
                    return make_pair(-work[a], a) < make_pair(-work[b], b);
                });
                if (order.size() > 24) order.resize(24);
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
                        if (!undo.empty()) moves.push_back({ledger.light_score(0.03), b, c});
                        for (auto it = undo.rbegin(); it != undo.rend(); ++it)
                            ledger.move_op(it->first, it->second);
                    }
                sort(moves.begin(), moves.end());
                for (size_t i = 0; i < min<size_t>(4, moves.size()); ++i) {
                    auto [cost, b, c] = moves[i];
                    (void)cost;
                    auto assignment = base.core;
                    for (int v : d.bundles[b]) assignment[v] = c;
                    SeedOptions opt;
                    opt.mode = round ? "dfs" : "critical";
                    opt.window = 8;
                    opt.memory = 0.5;
                    auto s = operation_plan(g, k, cfg, opt, &assignment, nullptr);
                    s.label = "retained_migration_" + to_string(round) + "_" + to_string(i);
                    offer(move(s));
                }
            }
        retained_seeds = legacy_pool;
        collecting_legacy = false;
    };
    if (!baseline_only) {
        const vector<tuple<double, int, double, bool>> variants = {
            {0.5, 0, 0.5, false}, {1.0, 0, 1.0, false}, {0.25, 0, 0.25, false},
            {0.5, 1, 1.0, false}, {1.0, 1, 2.0, false}, {0.25, 1, 0.5, false},
            {0.5, 2, 1.0, false}, {0.5, 3, 1.0, true},
            {0.25, 2, 2.0, true}, {1.0, 3, 4.0, true}};
        for (int i = 0; i < variant_limit; ++i) {
            auto [scale, cover, pack_comm, memory_cuts] = variants[i];
            auto d = segment(g, path_covers[cover], k, scale, memory_cuts);
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
            if (i + 1 == min(6, variant_limit)) retain_legacy_search();
        }
    }
    // Search whole segments, mandatory writer atoms, tensor neighborhoods and
    // two-segment swaps. No move ever separates a tensor's writers.
    if (!baseline_only && !pool.empty() && graph_work <= 200000) {
        vector<vector<int>> atoms(g.size());
        for (int v = 0; v < g.size(); ++v)
            atoms[g.writer_group[v]].push_back(v);
        auto close_writers = [&](vector<int> nodes) {
            vector<int> closed;
            for (int v : nodes)
                closed.insert(closed.end(), atoms[g.writer_group[v]].begin(),
                              atoms[g.writer_group[v]].end());
            Graph::unique_vec(closed);
            return closed;
        };
        for (int round = 0; round < 3; ++round) {
            Solution base = pool[min<size_t>(round, pool.size() - 1)].solution;
            const auto &d = decompositions[round % decompositions.size()];
            vector<vector<int>> groups = d.bundles;
            vector<int> order(groups.size());
            iota(order.begin(), order.end(), 0);
            vector<double> importance(order.size());
            for (size_t b = 0; b < d.bundles.size(); ++b)
                for (int v : groups[b])
                    importance[b] += g.weight(v);
            sort(order.begin(), order.end(), [&](int a, int b) {
                return make_pair(-importance[a], a) < make_pair(-importance[b], b);
            });
            if (order.size() > 24)
                order.resize(24);
            vector<vector<int>> neighborhoods;
            for (int b : order)
                neighborhoods.push_back(groups[b]);
            vector<int> tensors(g.tensors.size());
            iota(tensors.begin(), tensors.end(), 0);
            stable_sort(tensors.begin(), tensors.end(), [&](int a, int b) {
                auto cost = [&](int t) {
                    return double(g.tensors[t].size) *
                        double(g.tensors[t].consumers.size() + g.tensors[t].producers.size());
                };
                return make_pair(-cost(a), a) < make_pair(-cost(b), b);
            });
            for (size_t i = 0; i < min<size_t>(16, tensors.size()); ++i) {
                const auto &t = g.tensors[tensors[i]];
                auto uses = t.consumers;
                uses.insert(uses.end(), t.producers.begin(), t.producers.end());
                if (!uses.empty())
                    neighborhoods.push_back(close_writers(move(uses)));
            }
            vector<int> heavy = g.topo;
            stable_sort(heavy.begin(), heavy.end(), [&](int a, int b) {
                return make_pair(-g.weight(a), a) < make_pair(-g.weight(b), b);
            });
            set<int> used_atoms;
            for (int v : heavy)
                if (used_atoms.insert(g.writer_group[v]).second) {
                    neighborhoods.push_back(atoms[g.writer_group[v]]);
                    if (used_atoms.size() == 16)
                        break;
                }
            TrafficState ledger(g, cfg, k, base.core);
            struct Move { double score; vector<pair<int, int>> changes; };
            vector<Move> moves;
            auto price = [&](vector<pair<int, int>> changes) {
                vector<pair<int, int>> undo;
                for (auto [v, c] : changes)
                    if (ledger.core[v] != c) {
                        undo.push_back({v, ledger.core[v]});
                        ledger.move_op(v, c);
                    }
                if (!undo.empty())
                    moves.push_back({ledger.light_score(0.03), move(changes)});
                for (auto it = undo.rbegin(); it != undo.rend(); ++it)
                    ledger.move_op(it->first, it->second);
            };
            for (const auto &group : neighborhoods)
                for (int c = 0; c < k; ++c) {
                    vector<pair<int, int>> changes;
                    for (int v : group)
                        changes.push_back({v, c});
                    price(move(changes));
                }
            for (size_t i = 0; i < min<size_t>(8, order.size()); ++i)
                for (size_t j = i + 1; j < min<size_t>(8, order.size()); ++j) {
                    const auto &a = groups[order[i]], &b = groups[order[j]];
                    int ca = base.core[a.front()], cb = base.core[b.front()];
                    if (ca == cb)
                        continue;
                    vector<pair<int, int>> changes;
                    for (int v : a) changes.push_back({v, cb});
                    for (int v : b) changes.push_back({v, ca});
                    price(move(changes));
                }
            stable_sort(moves.begin(), moves.end(), [](const Move &a, const Move &b) {
                return make_pair(a.score, a.changes) < make_pair(b.score, b.changes);
            });
            for (size_t i = 0; i < min<size_t>(6, moves.size()); ++i) {
                auto a = base.core;
                for (auto [v, c] : moves[i].changes)
                    a[v] = c;
                SeedOptions opt;
                opt.mode = round ? "dfs" : "critical";
                opt.window = 8;
                opt.memory = 0.5;
                auto s = operation_plan(g, k, cfg, opt, &a, nullptr);
                s.label = "neighborhood_" + to_string(round) + "_" + to_string(i);
                offer(move(s));
            }
        }
        // Revisit placement independently of ordering. Larger memory weights
        // trade some early dispatch for shorter resident tensor lifetimes.
        vector<Solution> bases;
        for (size_t i = 0; i < min<size_t>(3, pool.size()); ++i)
            bases.push_back(pool[i].solution);
        for (const auto &base : bases)
            for (auto [mode, weight] : vector<pair<string, double>>{
                     {"dfs", 2.0}, {"critical", 4.0}, {"breadth", 1.0}, {"id", 2.0}}) {
                SeedOptions opt;
                opt.mode = mode;
                opt.window = 16;
                opt.memory = weight;
                auto s = operation_plan(g, k, cfg, opt, &base.core, nullptr);
                s.label = "memory_order_" + mode + "_" + base.label;
                offer(move(s));
            }
    }
    if (pool.empty())
        throw NoCandidate();
    for (const auto &seed : retained_seeds) {
        bool exists = false;
        for (const auto &e : pool)
            if (e.solution.core == seed.solution.core && e.solution.order == seed.solution.order)
                exists = true;
        if (!exists) pool.push_back(seed);
    }
    bool chosen = false;
    for (const auto &entry : pool) {
        auto refined = refine_estimate(g, entry.solution, k);
        ++result.refined_candidates;
        if (!refined.valid)
            continue;
        auto key = make_tuple(refined.cycles, refined.route + refined.spill,
                              entry.solution.core, entry.solution.order);
        auto best = make_tuple(result.refined.cycles, result.refined.route + result.refined.spill,
                               result.solution.core, result.solution.order);
        if (!chosen || key < best) {
            chosen = true;
            result.solution = entry.solution;
            result.estimate = entry.metrics;
            result.refined = move(refined);
        }
    }
    if (!chosen)
        throw NoCandidate();
    result.candidates = evaluated;
    result.repair_attempts = repair_attempts;
    result.generated_for_cores = k;
    return result;
}

Result solve(const Graph &g, int k, bool baseline_only) {
    if (k < 2 || k > 5)
        throw runtime_error("core count must be 2..5");
    Analysis analysis(g);
    vector<Decomposition> covers;
    for (bool recover : {false, true})
        for (double weight : {0.0, 0.25})
            covers.push_back(make_paths(g, analysis, weight, recover));
    if (baseline_only)
        return solve_at_core_count(g, k, true, covers);
    // N is the available number of cores, not a requirement to occupy them all.
    // Search independent core budgets concurrently, without changing candidates
    // or their within-portfolio order. Graph is read-only and every worker owns
    // its search state and indexed result slot. No shared pool/seen or nesting.
    array<optional<Result>, 4> completed;
    detail::run_independent_jobs(k - 1, [&](int index) {
        try {
            completed[index].emplace(solve_at_core_count(g, index + 2, false, covers));
        } catch (const NoCandidate &) {
            // Preserve v2: another core budget can still be feasible.
        }
    });
    // All threads have joined. Collect portfolios in ascending core-budget order;
    // completion order must never determine the incumbent or tie-breaking.
    vector<Result> portfolios;
    size_t evaluated = 0, repairs = 0, refined = 0;
    for (int index = 0; index < k - 1; ++index) {
        if (completed[index]) {
            auto candidate = move(*completed[index]);
            evaluated += candidate.candidates;
            repairs += candidate.repair_attempts;
            refined += candidate.refined_candidates;
            portfolios.push_back(move(candidate));
        }
    }
    if (portfolios.empty())
        throw NoCandidate();
    // Apply the same event-time criterion to every core budget. Smaller
    // portfolios are not judged by an incompatible pessimistic timing gate.
    Result best = move(portfolios.back());
    auto final_key = [](const Result &r) {
        return make_tuple(r.refined.cycles, r.refined.route + r.refined.spill,
                          r.generated_for_cores, r.solution.core, r.solution.order);
    };
    for (int i = int(portfolios.size()) - 2; i >= 0; --i)
        if (final_key(portfolios[i]) < final_key(best))
            best = move(portfolios[i]);
    best.candidates = evaluated;
    best.repair_attempts = repairs;
    best.refined_candidates = refined;
    validate(g, best.solution, k);
    return best;
}
} // namespace dag_chain
