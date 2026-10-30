#include "chain.hpp"
#include "spill.hpp"
#include "search_limits.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

// Logical IDs are deliberately independent of file order and external node IDs.
// This builder never uses any public benchmark data.
struct Fixture {
    vector<int> cycles;
    vector<i64> sizes;
    vector<vector<int>> writers, readers;
    vector<pair<int, int>> dependencies;
    string memory = "UB";
};
i64 op_id(int v, bool relabel) { return relabel ? 900001 - 97 * v : v; }
i64 tensor_id(int t, bool relabel) { return relabel ? 700003 + 101 * t : 1000 + t; }

Graph make_graph(const Fixture &f, bool relabel) {
    ostringstream out;
    out << "{\"ops\":[";
    for (int i = 0; i < int(f.cycles.size()); ++i) {
        const int v = relabel ? int(f.cycles.size()) - 1 - i : i;
        if (i) out << ',';
        out << "{\"id\":" << op_id(v, relabel)
            << ",\"op\":\"COMPUTE\",\"pipe\":\"PIPE_V\",\"cycles\":" << f.cycles[v] << '}';
    }
    out << "],\"tensors\":[";
    for (int i = 0; i < int(f.sizes.size()); ++i) {
        const int t = relabel ? int(f.sizes.size()) - 1 - i : i;
        if (i) out << ',';
        out << "{\"id\":" << tensor_id(t, relabel) << ",\"pos\":\"" << f.memory
            << "\",\"size\":" << f.sizes[t] << '}';
    }
    out << "],\"edges\":[";
    vector<pair<i64, i64>> edges;
    for (int t = 0; t < int(f.sizes.size()); ++t) {
        for (int v : f.writers[t]) edges.push_back({op_id(v, relabel), tensor_id(t, relabel)});
        for (int v : f.readers[t]) edges.push_back({tensor_id(t, relabel), op_id(v, relabel)});
    }
    for (auto [a, b] : f.dependencies) edges.push_back({op_id(a, relabel), op_id(b, relabel)});
    if (relabel) reverse(edges.begin(), edges.end());
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i) out << ',';
        out << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    out << "]}";
    const string input = out.str();
    return Graph(JsonParser(input).parse());
}

Solution producer_first(const Graph &g, bool relabel) {
    Solution s;
    s.core.assign(g.size(), 0);
    for (int v = 0; v < g.size(); ++v) {
        const auto found = find_if(g.ops.begin(), g.ops.end(), [&](const Op &op) {
            return op.id == op_id(v, relabel);
        });
        require(found != g.ops.end(), "fixture operation not found");
        s.order.push_back(int(found - g.ops.begin()));
    }
    s.label = "deliberate_long_lifetimes";
    return s;
}

string fingerprint(const Graph &g) {
    ostringstream out;
    for (const auto &op : g.ops) {
        out << op.id << ':' << op.cycles << ':' << op.pipe << ';';
        for (const auto *row : {&op.pred, &op.succ, &op.inputs, &op.outputs}) {
            for (int v : *row) out << v << ',';
            out << '|';
        }
    }
    for (const auto &t : g.tensors) {
        out << t.id << ':' << t.pos << ':' << t.size << ':' << t.final_output;
        for (int v : t.producers) out << 'w' << v;
        for (int v : t.consumers) out << 'r' << v;
    }
    for (const auto &e : g.direct) out << 'e' << e.a << ':' << e.b << ':' << e.size;
    for (int v : g.writer_group) out << 'g' << v;
    return out.str();
}

vector<Solution> check_candidates(const Graph &g, const Solution &seed, int cores) {
    const auto original = fingerprint(g);
    const auto input_plan = solution_json(g, seed, cores);
    validate(g, seed, cores);
    auto candidates = dag_chain::spill_candidates(g, cores, seed);
    const auto again = dag_chain::spill_candidates(g, cores, seed);
    require(candidates.size() == again.size(), "spill proposal count is nondeterministic");
    require(candidates.size() <= dag_chain::search_limits::spill_max_candidates,
            "spill proposals exceeded the configured search cap");
    for (size_t i = 0; i < candidates.size(); ++i) {
        validate(g, candidates[i], cores);
        require(solution_json(g, candidates[i], cores) == solution_json(g, again[i], cores),
                "fixed spill proposals must repeat identically");
        for (int v = 0; v < g.size(); ++v)
            require(candidates[i].core[v] == candidates[i].core[g.writer_group[v]],
                    "spill optimization split an atomic writer group");
    }
    require(original == fingerprint(g), "spill optimization changed original graph constraints");
    require(input_plan == solution_json(g, seed, cores), "spill optimization mutated its incumbent");
    return candidates;
}

void pressure_improvement(const string &memory, bool relabel) {
    // Three independent 80 KB (UB) / 900 KB (L1) producers followed by all
    // consumers force spills. Finishing a ready consumer before the next
    // producer removes that pressure without adding/deleting any dependency.
    const i64 bytes = memory == "UB" ? 80000 : 900000;
    Fixture fixture{{50, 50, 50, 50, 50, 50}, {bytes, bytes, bytes},
                    {{0}, {1}, {2}}, {{3}, {4}, {5}}, {}, memory};
    const auto g = make_graph(fixture, relabel);
    const auto seed = producer_first(g, relabel);
    const Config cfg;
    const auto before = estimate_spills(g, cfg, seed, 2, nullptr);
    require(!before.infeasible && before.bytes > 0, "fixture must expose avoidable spill traffic");
    auto candidates = check_candidates(g, seed, 2);
    bool reduced_with_same_assignment = false;
    for (const auto &candidate : candidates) {
        const auto after = estimate_spills(g, cfg, candidate, 2, nullptr);
        if (candidate.core == seed.core && !after.infeasible && after.bytes < before.bytes)
            reduced_with_same_assignment = true;
    }
    require(reduced_with_same_assignment,
            "ready-consumer reordering should reduce spill on the independent pressure fixture");
}
} // namespace

int main() {
    try {
        for (const string &memory : {string("UB"), string("L1")})
            for (bool relabel : {false, true}) pressure_improvement(memory, relabel);

        // Includes partial writes separated by other work, fanout, and a
        // redundant direct edge. All of them must survive ordering/moving.
        Fixture writers{{20, 20, 20, 20, 20, 20, 20, 20},
                        {80000, 60000, 60000}, {{0, 3}, {1}, {4}},
                        {{6, 7}, {2}, {5}}, {{0, 3}, {2, 3}, {3, 4}, {5, 6}, {0, 7}}, "UB"};
        Fixture control_only{{0, 20, 0, 30, 1}, {}, {}, {},
                             {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {0, 3}, {3, 4}}, "UB"};
        for (bool relabel : {false, true}) {
            for (const auto *fixture : {&writers, &control_only}) {
                const auto g = make_graph(*fixture, relabel);
                const auto seed = producer_first(g, relabel);
                for (int cores = 2; cores <= 5; ++cores) check_candidates(g, seed, cores);
            }
        }

        // New proposals retain the complete v5 incumbent under the same
        // estimator. This assertion does not claim universal official gains.
        const auto g = make_graph(writers, false);
        const auto v5 = dag_chain::solve(g, 3, false, 3);
        const auto v7 = dag_chain::solve(g, 3, false, 7);
        require(v5.refined.valid && v7.refined.valid, "integration requires complete feasible estimates");
        require(make_pair(v7.refined.cycles, v7.refined.route + v7.refined.spill) <=
                    make_pair(v5.refined.cycles, v5.refined.route + v5.refined.spill),
                "spill extension discarded a better complete v5 incumbent");
        const auto repeated = dag_chain::solve(g, 3, false, 7);
        require(solution_json(g, v7.solution, 3) == solution_json(g, repeated.solution, 3),
                "parallel fixed-work solve is nondeterministic");
        bool rejected = false;
        try { (void)dag_chain::solve(g, 2, false, 8); }
        catch (const runtime_error &) { rejected = true; }
        require(rejected, "unknown extension flags must be rejected");
        cout << "spill pressure reduction, writer/dependency preservation, relabeling and determinism passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}

