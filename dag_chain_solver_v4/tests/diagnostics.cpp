#include "chain.hpp"
Solution load_singleton_plan(const Graph &g, const fs::path &path, int k) {
    Json json = JsonParser(read_text(path)).parse();
    map<i64, int> index;
    for (int v = 0; v < g.size(); ++v) index[g.ops[v].id] = v;
    Solution s;
    s.core.assign(g.size(), -1);
    s.label = "external_plan";
    vector<vector<int>> successors(g.size());
    vector<int> degree(g.size());
    for (int v = 0; v < g.size(); ++v) successors[v] = g.ops[v].succ;
    const auto &streams = json.at("core_schedules").array();
    if (int(streams.size()) != k) throw runtime_error("wrong core count");
    for (int c = 0; c < k; ++c) {
        int last = -1;
        for (const auto &item : streams[c].array()) {
            int v = index.at(item.integer());
            if (s.core[v] >= 0) throw runtime_error("duplicate op");
            s.core[v] = c;
            if (last >= 0) successors[last].push_back(v);
            last = v;
        }
    }
    for (auto &row : successors) {
        Graph::unique_vec(row);
        for (int v : row) ++degree[v];
    }
    priority_queue<int, vector<int>, greater<int>> ready;
    for (int v = 0; v < g.size(); ++v) if (!degree[v]) ready.push(v);
    while (!ready.empty()) {
        int v = ready.top(); ready.pop(); s.order.push_back(v);
        for (int w : successors[v]) if (!--degree[w]) ready.push(w);
    }
    validate(g, s, k);
    return s;
}
int main(int argc, char **argv) {
    try {
        if (argc < 3) throw runtime_error("usage: chain_diagnostics graph.json cores [plan.json]");
        Graph g(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        int k = stoi(argv[2]);
        if (argc == 5 && string(argv[3]) == "--evaluate-plan") {
            auto s = load_singleton_plan(g, fs::u8path(argv[4]), k);
            auto r = dag_chain::refine_estimate(g, s, k);
            cout << setprecision(15) << "{\"valid\":" << (r.valid ? "true" : "false")
                 << ",\"estimated_cycles\":" << r.cycles << ",\"route\":" << r.route
                 << ",\"spill\":" << r.spill << "}\n";
            return 0;
        }
        auto start = Clock::now();
        auto r = dag_chain::solve(g, k);
        double seconds = chrono::duration<double>(Clock::now() - start).count();
        cout << setprecision(15) << "{\"estimated_cycles\":" << r.refined.cycles
             << ",\"estimated_route_bytes\":" << r.refined.route
             << ",\"estimated_spill_bytes\":" << r.refined.spill
             << ",\"coarse_sketch\":" << r.estimate.sketch
             << ",\"coarse_spill\":" << r.estimate.spill
             << ",\"candidates\":" << r.candidates
             << ",\"refined_candidates\":" << r.refined_candidates
             << ",\"effective_cores\":" << r.generated_for_cores
             << ",\"label\":\"" << r.solution.label << "\",\"seconds\":" << seconds << "}\n";
        if (argc > 3) {
            auto path = fs::u8path(argv[3]);
            if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
            ofstream out(path);
            if (!out) throw runtime_error("cannot create diagnostic plan");
            out << solution_json(g, r.solution, k);
            if (!out) throw runtime_error("cannot write diagnostic plan");
        }
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
