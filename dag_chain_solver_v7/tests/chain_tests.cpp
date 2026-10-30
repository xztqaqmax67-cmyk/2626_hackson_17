#include "chain.hpp"

void require(bool ok, const string &message) {
    if (!ok)
        throw runtime_error(message);
}
Graph sample(const vector<int> &cycles, const vector<pair<int, int>> &edges) {
    ostringstream s;
    s << "{\"tensors\":[],\"ops\":[";
    for (size_t i = 0; i < cycles.size(); ++i) {
        if (i)
            s << ',';
        s << "{\"id\":" << i << ",\"op\":\"M\",\"pipe\":\"PIPE_M\",\"cycles\":" << cycles[i] << '}';
    }
    s << "],\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i)
            s << ',';
        s << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    s << "]}";
    string text = s.str();
    return Graph(JsonParser(text).parse());
}
void verify_paths(const Graph &g, const dag_chain::Decomposition &d) {
    vector<int> count(g.size());
    for (const auto &path : d.paths)
        for (size_t i = 0; i < path.size(); ++i) {
            int v = path[i];
            ++count[v];
            if (i)
                require(find(g.ops[path[i - 1]].succ.begin(), g.ops[path[i - 1]].succ.end(), v) !=
                            g.ops[path[i - 1]].succ.end(),
                        "path uses a nonexistent edge");
        }
    for (int c : count)
        require(c == 1, "paths must be a disjoint cover");
    vector<int> bundled(g.size());
    for (const auto &b : d.bundles)
        for (int v : b)
            ++bundled[v];
    for (int c : bundled)
        require(c == 1, "bundles must be a disjoint cover");
}
int main() {
    try {
        // A slow upstream chain outranks a predecessor with a long individual op.
        auto g = sample({100, 10, 200, 1}, {{2, 1}, {0, 3}, {1, 3}});
        auto d = dag_chain::decompose(g, 2, 1);
        require(d.parent[3] == 1, "parent selection must use cumulative finish time");
        verify_paths(g, d);
        auto fork = sample({10, 5, 4, 2}, {{0, 1}, {0, 2}, {1, 3}, {2, 3}});
        auto f = dag_chain::decompose(fork, 2, 1);
        verify_paths(fork, f);
        require(fork.ops[3].pred.size() == 2, "non-chain dependencies were discarded");
        auto line = sample({20, 20, 20, 20, 20, 20}, {{0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}});
        auto l = dag_chain::decompose(line, 2, 0.25);
        verify_paths(line, l);
        require(l.paths.size() == 1 && l.bundles.size() > 1, "heavy chains must be splittable");
        // Opposite cross edges make contracting two otherwise valid paths cyclic.
        auto crossing = sample({10, 10, 10, 10}, {{0, 2}, {0, 3}, {1, 2}, {1, 3}});
        require(dag_chain::decompose(crossing, 2, 1).paths.size() == 2,
                "a child losing its first parent should recover a free predecessor");
        for (int k = 2; k <= 5; ++k) {
            auto a = dag_chain::solve(crossing, k), b = dag_chain::solve(crossing, k);
            validate(crossing, a.solution, k);
            require(solution_json(crossing, a.solution, k) ==
                        solution_json(crossing, b.solution, k),
                    "output must be deterministic");
        }
        auto empty = sample({}, {});
        require(dag_chain::solve(empty, 4).solution.order.empty(), "empty DAG");
        // Scene B charges a distinct write/read pair per remote destination.
        string tensor_json =
            R"({"tensors":[{"id":10,"pos":"UB","size":600}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":10},{"id":1,"op":"V","pipe":"PIPE_V","cycles":10},{"id":2,"op":"V","pipe":"PIPE_V","cycles":10}],"edges":[{"source":0,"target":10},{"source":10,"target":1},{"source":10,"target":2}]})";
        Graph tg(JsonParser(tensor_json).parse());
        Config cfg;
        TrafficState traffic(tg, cfg, 3, {0, 1, 2});
        require(traffic.bytes == 2400, "per-destination traffic accounting");
        Placement timeline(tg, 3, cfg);
        timeline.commit(timeline.trial(0, 0));
        auto consumer = timeline.trial(1, 1);
        require(consumer.start == 1030, "COPY_OUT then delay then COPY_IN");
        string partial =
            R"({"tensors":[{"id":10,"pos":"UB","size":80000},{"id":11,"pos":"UB","size":80000}],"ops":[{"id":0,"op":"M","pipe":"PIPE_M","cycles":10},{"id":1,"op":"M","pipe":"PIPE_M","cycles":10},{"id":2,"op":"M","pipe":"PIPE_M","cycles":10},{"id":3,"op":"M","pipe":"PIPE_M","cycles":10},{"id":4,"op":"M","pipe":"PIPE_M","cycles":10}],"edges":[{"source":0,"target":10},{"source":3,"target":10},{"source":10,"target":4},{"source":1,"target":11},{"source":11,"target":2}]})";
        Graph pg(JsonParser(partial).parse());
        Solution ps;
        ps.core = {0, 0, 0, 0, 0};
        ps.order = {0, 1, 2, 3, 4};
        require(!estimate_spills(pg, cfg, ps, 2, nullptr).infeasible,
                "updated Step 2 supports spills between partial writes");
        auto unsafe = estimate_spills(pg, cfg, ps, 2, nullptr);
        require(unsafe.partial_write_victims == vector<int>{0},
                "identify the tensor whose partial writes are interrupted");
        auto repaired = dag_chain::repair_writer_order(pg, ps, unsafe.partial_write_victims);
        validate(pg, repaired, 2);
        require(repaired.core == ps.core, "repair must preserve core placement");
        require(!estimate_spills(pg, cfg, repaired, 2, nullptr).infeasible,
                "repair should finish partial writes before unrelated allocations");
        require(estimate_spills(pg, cfg, repaired, 2, nullptr).partial_write_victims.empty(),
                "clustering should complete writes before eviction; total traffic may tie");
        require(dag_chain::repair_writer_order(pg, ps, {}).order == ps.order,
                "no reported hazard should preserve the original topological order");
        // The second writer needs another branch first. Repair must bring its
        // prerequisites forward, not violate their dependency or open A early.
        string dependent_partial = partial;
        dependent_partial.insert(dependent_partial.size() - 2, ",{\"source\":2,\"target\":3}");
        Graph dependent(JsonParser(dependent_partial).parse());
        auto deps = estimate_spills(dependent, cfg, ps, 2, nullptr);
        auto prepared = dag_chain::repair_writer_order(dependent, ps, deps.partial_write_victims);
        validate(dependent, prepared, 2);
        require(!estimate_spills(dependent, cfg, prepared, 2, nullptr).infeasible,
                "repair must prepare future writers' prerequisites before opening the output");
        auto two = dag_chain::solve(crossing, 2);
        auto five = dag_chain::solve(crossing, 5);
        validate(crossing, five.solution, 5);
        require(five.candidates >= two.candidates,
                "larger available core count must consider smaller-core portfolios");
        require(five.generated_for_cores >= 2 && five.generated_for_cores <= 5,
                "track the selected portfolio's effective core budget");
        require(two.refined.valid && five.refined.valid && five.refined.cycles <= two.refined.cycles,
                "additional available cores must preserve the best smaller-budget estimate");
        cout << "chain invariants, segmentation, dependency preservation, determinism and transfer "
                "estimates: passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
