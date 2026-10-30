#include "regions.hpp"
#include <functional>

namespace dag_chain {
namespace {
int bit_count(unsigned mask) {
    int result = 0;
    for (; mask; mask &= mask - 1U) ++result;
    return result;
}
double maximum(const Row &row) {
    return *max_element(row.begin(), row.end());
}
struct RegionWork {
    Row work{};
    double critical = 0;
    unsigned forced_mask = 0;
    vector<double> affinity;
};
struct Atom {
    vector<int> nodes, tensors, direct;
    Row work{};
    int forced = -1;
    double critical = 0;
};

// Chain continuity is a preference, whereas writer groups are an invariant.
// In particular, joining a chain must never merge conflicting pinned writers.
vector<Atom> region_atoms(const Graph &g, const vector<int> &nodes,
                          const vector<int> &next, const vector<int> &forced,
                          int quota, vector<int> &local_index) {
    const int n = int(nodes.size());
    for (int i = 0; i < n; ++i) local_index[nodes[i]] = i;
    DSU groups(n);
    vector<int> pin(n, -1), first(g.size(), -1);
    Row work{};
    for (int i = 0; i < n; ++i) {
        int v = nodes[i], writer = g.writer_group[v];
        pin[i] = forced[writer];
        if (first[writer] >= 0) groups.join(first[writer], i);
        else first[writer] = i;
        work[g.ops[v].pipe] += g.weight(v);
    }
    for (int i = 0; i < n; ++i)
        if (pin[i] >= 0) pin[groups.get(i)] = pin[i];
    const double target = max(1.0, maximum(work) / quota * 0.5);
    vector<int> incoming(n, -1);
    for (int i = 0; i < n; ++i) {
        int w = next[nodes[i]];
        if (w >= 0 && local_index[w] >= 0) incoming[local_index[w]] = i;
    }
    for (int i = 0; i < n; ++i) if (incoming[i] < 0) {
        Row segment{};
        int previous = -1;
        for (int v = nodes[i]; v >= 0 && local_index[v] >= 0; v = next[v]) {
            int index = local_index[v], pipe = g.ops[v].pipe;
            bool joined = false;
            if (previous >= 0 && segment[pipe] + g.weight(v) <= target) {
                int a = groups.get(previous), b = groups.get(index);
                if (pin[a] < 0 || pin[b] < 0 || pin[a] == pin[b]) {
                    int preserved = max(pin[a], pin[b]);
                    groups.join(a, b);
                    pin[groups.get(a)] = preserved;
                    joined = true;
                }
            }
            if (!joined) segment = Row{};
            segment[pipe] += g.weight(v);
            previous = index;
        }
    }
    vector<Atom> atoms;
    map<int, int> indices;
    for (int i = 0; i < n; ++i) {
        int v = nodes[i], representative = groups.get(i);
        if (!indices.count(representative)) {
            indices[representative] = int(atoms.size());
            atoms.emplace_back();
            atoms.back().forced = pin[representative];
        }
        auto &atom = atoms[indices[representative]];
        atom.nodes.push_back(v);
        atom.work[g.ops[v].pipe] += g.weight(v);
        atom.critical = max(atom.critical, g.rank[v]);
        atom.tensors.insert(atom.tensors.end(), g.ops[v].inputs.begin(), g.ops[v].inputs.end());
        atom.tensors.insert(atom.tensors.end(), g.ops[v].outputs.begin(), g.ops[v].outputs.end());
        atom.direct.insert(atom.direct.end(), g.ops[v].direct_inc.begin(), g.ops[v].direct_inc.end());
    }
    for (auto &atom : atoms) {
        Graph::unique_vec(atom.tensors);
        Graph::unique_vec(atom.direct);
    }
    auto priority = [&](const Atom &atom) {
        double pressure = 0;
        for (int p = 0; p < 5; ++p)
            pressure = max(pressure, atom.work[p] / max(1.0, work[p] / quota));
        return make_tuple(-pressure, -atom.critical, atom.nodes.front());
    };
    stable_sort(atoms.begin(), atoms.end(), [&](const Atom &a, const Atom &b) {
        return priority(a) < priority(b);
    });
    for (int v : nodes) local_index[v] = -1;
    return atoms;
}

double touched_routes(const Graph &g, const TrafficState &ledger, const Atom &atom) {
    double routes = 0;
    for (int t : atom.tensors) {
        int producers = 0, consumers = 0, both = 0;
        for (int c = 0; c < ledger.k; ++c) {
            bool p = ledger.pc[size_t(t) * ledger.k + c] > 0;
            bool q = ledger.cc[size_t(t) * ledger.k + c] > 0;
            producers += int(p); consumers += int(q); both += int(p && q);
        }
        routes += producers * consumers - both;
    }
    for (int index : atom.direct) {
        const auto &edge = g.direct[index];
        routes += ledger.core[edge.a] != ledger.core[edge.b];
    }
    return routes;
}

void place_region(const Graph &g, const vector<int> &nodes, unsigned mask,
                  const vector<int> &next, const vector<int> &forced,
                  TrafficState &ledger, vector<int> &local_index) {
    const int quota = bit_count(mask);
    auto atoms = region_atoms(g, nodes, next, forced, quota, local_index);
    Row total{};
    vector<Row> load(ledger.k);
    for (const auto &atom : atoms)
        for (int p = 0; p < 5; ++p) total[p] += atom.work[p];
    const Config cfg;
    for (const auto &atom : atoms) {
        vector<int> previous;
        for (int v : atom.nodes) previous.push_back(ledger.core[v]);
        const double before_bytes = ledger.bytes;
        const double before_routes = touched_routes(g, ledger, atom);
        tuple<double, double, int> best_key{INF, INF, 0};
        int best = -1;
        for (int c = 0; c < ledger.k; ++c) {
            if (!(mask & (1U << c)) || (atom.forced >= 0 && c != atom.forced)) continue;
            for (int v : atom.nodes) ledger.move_op(v, c);
            double balance = 0;
            for (int p = 0; p < 5; ++p) {
                double w = atom.work[p];
                balance += (2 * load[c][p] * w + w * w) / max(1.0, total[p] / quota);
            }
            // Global tensor accounting includes boundary users and direct edges.
            // This remains a placement heuristic, not a bandwidth simulation.
            double communication = (ledger.bytes - before_bytes) / cfg.bandwidth +
                (touched_routes(g, ledger, atom) - before_routes) * cfg.delay;
            auto key = make_tuple(balance + communication, ledger.bytes, c);
            if (key < best_key) { best_key = key; best = c; }
            for (size_t i = atom.nodes.size(); i > 0; --i)
                ledger.move_op(atom.nodes[i - 1], previous[i - 1]);
        }
        if (best < 0) throw runtime_error("region placement lost a writer-compatible core");
        for (int v : atom.nodes) ledger.move_op(v, best);
        for (int p = 0; p < 5; ++p) load[best][p] += atom.work[p];
    }
}
RegionPartition partition_at_cuts(const Graph &g, const vector<double> &depth,
                                  const vector<double> &cuts) {
    const int bands = int(cuts.size()) + 1;
    RegionPartition result;
    result.phase_of.assign(g.size(), 0);
    result.components.resize(size_t(bands));
    vector<vector<int>> nodes(size_t(bands), vector<int>{});
    for (int v : g.topo) {
        int phase = int(upper_bound(cuts.begin(), cuts.end(), depth[v]) - cuts.begin());
        result.phase_of[v] = phase;
        nodes[phase].push_back(v);
    }
    DSU connected(g.size());
    for (int v : g.topo)
        for (int u : g.ops[v].pred)
            if (result.phase_of[u] == result.phase_of[v]) connected.join(u, v);
    vector<int> writer_first(g.size(), -1);
    for (int phase = 0; phase < bands; ++phase) {
        for (int v : nodes[phase]) {
            int writer = g.writer_group[v];
            if (writer_first[writer] >= 0) connected.join(writer_first[writer], v);
            else writer_first[writer] = v;
        }
        map<int, int> index;
        for (int v : nodes[phase]) {
            int root = connected.get(v);
            if (!index.count(root)) {
                index[root] = int(result.components[phase].size());
                result.components[phase].push_back({});
            }
            result.components[phase][index[root]].push_back(v);
            writer_first[g.writer_group[v]] = -1;
        }
    }
    return result;
}
void weighted_depth(const Graph &g, vector<double> &depth, vector<double> &finish) {
    depth.assign(g.size(), 0);
    finish.assign(g.size(), 0);
    for (int v : g.topo) {
        for (int u : g.ops[v].pred) depth[v] = max(depth[v], finish[u]);
        finish[v] = depth[v] + g.weight(v);
    }
}
} // namespace

RegionPartition make_region_partition(const Graph &g, int bands) {
    if (bands < 1) throw runtime_error("region band count must be positive");
    vector<double> depth, finish, cuts;
    weighted_depth(g, depth, finish);
    double horizon = 1;
    for (double end : finish) horizon = max(horizon, end);
    for (int i = 1; i < bands; ++i) cuts.push_back(horizon * (double(i) / bands));
    return partition_at_cuts(g, depth, cuts);
}

RegionPartition make_fork_join_partition(const Graph &g) {
    vector<double> depth, finish, cuts;
    weighted_depth(g, depth, finish);
    double horizon = 1;
    for (double end : finish) horizon = max(horizon, end);
    vector<tuple<double, double, int>> forks, joins;
    for (int v : g.topo) {
        if (g.ops[v].succ.size() > 1) {
            double sum = 0, largest = 0;
            for (int w : g.ops[v].succ) {
                sum += g.rank[w];
                largest = max(largest, g.rank[w]);
            }
            forks.emplace_back(-(sum - largest), finish[v], v);
        }
        if (g.ops[v].pred.size() > 1) {
            double sum = 0, largest = 0;
            for (int u : g.ops[v].pred) {
                sum += g.weight(u);
                largest = max(largest, g.weight(u));
            }
            joins.emplace_back(-(sum - largest), depth[v], v);
        }
    }
    // At most two fork and two join boundaries. Their impact ranks use only the
    // input graph. A common fork/join remains outside its naturally split region;
    // ordinary weighted bands can otherwise hide even two independent long ops.
    for (auto *candidates : {&forks, &joins}) {
        sort(candidates->begin(), candidates->end());
        int accepted = 0;
        for (const auto &candidate : *candidates) {
            double cut = get<1>(candidate);
            if (!(cut > 0 && cut < horizon) || find(cuts.begin(), cuts.end(), cut) != cuts.end()) continue;
            cuts.push_back(cut);
            if (++accepted == 2) break;
        }
    }
    sort(cuts.begin(), cuts.end());
    return partition_at_cuts(g, depth, cuts);
}

vector<vector<int>> enumerate_core_budgets(int blocks, int cores) {
    vector<vector<int>> result;
    if (blocks < 1 || cores < blocks || cores > 5) return result;
    vector<int> budget(size_t(blocks), 1);
    function<void(int, int)> visit = [&](int i, int remaining) {
        if (i == blocks) { result.push_back(budget); return; }
        for (int q = 1; q <= remaining - (blocks - i - 1); ++q) {
            budget[i] = q;
            visit(i + 1, remaining - q);
        }
    };
    visit(0, cores);
    return result;
}

vector<Solution> region_candidates(const Graph &g, int k, const Solution &seed,
                                   RegionStats *stats) {
    if (k < 2 || k > 5) throw runtime_error("region core count must be 2..5");
    validate(g, seed, k);
    vector<Solution> result;
    const size_t graph_work = size_t(g.size()) + g.edge_count + g.tensors.size();
    if (graph_work > 200000) return result;
    const Config cfg;
    // Reuse v4's path preference only; its contracted bundles are never executed.
    auto decomposition = decompose(g, k, 0.5, 0.25);
    // Zero denotes the bounded natural fork/join boundary proposal.
    const vector<int> bands = graph_work > 80000 ? vector<int>{2} : vector<int>{2, 4, 8, 0};
    set<vector<int>> seen;
    for (int count : bands) {
        auto partition = count ? make_region_partition(g, count) : make_fork_join_partition(g);
        if (stats) ++stats->partitions;
        vector<int> writer_phase(g.size(), -1), forced(g.size(), -1);
        for (int v : g.topo) {
            int writer = g.writer_group[v], phase = partition.phase_of[v];
            if (writer_phase[writer] < 0) writer_phase[writer] = phase;
            else if (writer_phase[writer] != phase) forced[writer] = seed.core[v];
        }
        TrafficState ledger(g, cfg, k, seed.core);
        vector<int> component_of(g.size(), -1), local_index(g.size(), -1);
        vector<double> finish(g.size());
        bool changed = false;
        for (int phase = 0; phase < int(partition.components.size()); ++phase) {
            const auto &components = partition.components[phase];
            const int m = int(components.size());
            // Too many natural components need sharing rather than disjoint masks;
            // leave that phase to the preserved v4 assignment.
            if (m < 2 || m > k) continue;
            if (stats) ++stats->split_phases;
            for (int i = 0; i < m; ++i)
                for (int v : components[i]) component_of[v] = i;
            vector<RegionWork> work(size_t(m), RegionWork{});
            for (int i = 0; i < m; ++i) {
                work[i].affinity.assign(size_t(k), 0);
                set<int> touched;
                for (int v : components[i]) {
                    work[i].work[g.ops[v].pipe] += g.weight(v);
                    double start = 0;
                    for (int u : g.ops[v].pred)
                        if (component_of[u] == i) start = max(start, finish[u]);
                    finish[v] = start + g.weight(v);
                    work[i].critical = max(work[i].critical, finish[v]);
                    int pin = forced[g.writer_group[v]];
                    if (pin >= 0) work[i].forced_mask |= 1U << pin;
                    touched.insert(g.ops[v].inputs.begin(), g.ops[v].inputs.end());
                    touched.insert(g.ops[v].outputs.begin(), g.ops[v].outputs.end());
                }
                for (int t : touched) {
                    unsigned outside = 0;
                    for (int v : g.tensors[t].producers)
                        if (component_of[v] != i) outside |= 1U << ledger.core[v];
                    for (int v : g.tensors[t].consumers)
                        if (component_of[v] != i) outside |= 1U << ledger.core[v];
                    for (int c = 0; c < k; ++c)
                        if (outside & (1U << c))
                            work[i].affinity[c] += 2.0 * double(g.tensors[t].size) / cfg.bandwidth + cfg.delay;
                }
                set<int> direct;
                for (int v : components[i])
                    direct.insert(g.ops[v].direct_inc.begin(), g.ops[v].direct_inc.end());
                for (int index : direct) {
                    const auto &edge = g.direct[index];
                    if ((component_of[edge.a] == i) == (component_of[edge.b] == i)) continue;
                    int outside = component_of[edge.a] == i ? edge.b : edge.a;
                    work[i].affinity[ledger.core[outside]] += 2.0 * double(edge.size) / cfg.bandwidth + cfg.delay;
                }
            }
            const auto budgets = enumerate_core_budgets(m, k);
            if (stats) stats->budget_vectors += budgets.size();
            vector<unsigned> selected(size_t(m), 0), best;
            tuple<double, double, int, vector<unsigned>> best_key{INF, INF, 0, {}};
            for (const auto &budget : budgets) {
                function<void(int, unsigned)> assign = [&](int i, unsigned used) {
                    if (i == m) {
                        if (stats) ++stats->mask_assignments;
                        double stage_time = 0, boundary = 0;
                        for (int j = 0; j < m; ++j) {
                            stage_time = max(stage_time, max(work[j].critical, maximum(work[j].work) / budget[j]));
                            for (int c = 0; c < k; ++c)
                                if (!(selected[j] & (1U << c))) boundary += work[j].affinity[c];
                        }
                        // A mask is a resource allowance, not a guarantee all its
                        // cores are used. Full-graph replay later accounts for DDR.
                        auto key = make_tuple(stage_time + 0.25 * boundary, boundary, bit_count(used), selected);
                        if (key < best_key) { best_key = key; best = selected; }
                        return;
                    }
                    for (unsigned mask = 1; mask < (1U << k); ++mask) {
                        if ((mask & used) || bit_count(mask) != budget[i] ||
                            (mask & work[i].forced_mask) != work[i].forced_mask) continue;
                        selected[i] = mask;
                        assign(i + 1, used | mask);
                    }
                };
                assign(0, 0);
            }
            if (!best.empty()) {
                for (int i = 0; i < m; ++i)
                    place_region(g, components[i], best[i], decomposition.next, forced, ledger, local_index);
                changed = true;
            }
            for (const auto &nodes : components)
                for (int v : nodes) component_of[v] = -1;
        }
        if (!changed || ledger.core == seed.core || !seen.insert(ledger.core).second) continue;
        for (int mode = 0; mode < (graph_work > 80000 ? 1 : 2); ++mode) {
            SeedOptions options;
            options.mode = mode ? "dfs" : "critical";
            options.window = 8;
            options.memory = 0.5;
            auto candidate = operation_plan(g, k, cfg, options, &ledger.core, nullptr);
            candidate.label = (count ? "region_bands" + to_string(count) : string("region_fork_join")) + "_" + options.mode;
            validate(g, candidate, k);
            result.push_back(move(candidate));
        }
    }
    return result;
}
} // namespace dag_chain
