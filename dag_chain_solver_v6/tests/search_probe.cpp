#include "anytime.hpp"
Solution load_singleton_plan(const Graph &g, const fs::path &path, int k) {
    Json json = JsonParser(read_text(path)).parse();
    map<i64, int> index;
    for (int v = 0; v < g.size(); ++v) index[g.ops[v].id] = v;
    // Core schedules name subgraphs, so accepting a grouped or relabelled plan
    // as operator IDs would silently evaluate a different warm-start seed.
    const auto &mapping = json.at("node_to_subgraph");
    if (mapping.type != Json::Object || mapping.fields.size() != size_t(g.size()))
        throw runtime_error("resume requires a complete singleton identity mapping");
    for (const auto &entry : mapping.fields) {
        const i64 id = entry.second.integer();
        if (entry.first != to_string(id) || index.count(id) == 0)
            throw runtime_error("resume requires a singleton identity mapping");
    }
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
        if (argc != 7 && argc != 8)
            throw runtime_error("usage: search_probe graph cores seconds policy(0=legacy,1=useful_memory) full/resume output [seed]");
        const auto began = Clock::now();
        Graph g(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        const int cores = stoi(argv[2]);
        const double seconds = stod(argv[3]);
        dag_chain::TimedSearchOptions options;
        const string policy = argv[4];
        if (policy != "0" && policy != "1") throw runtime_error("policy must be 0 or 1");
        options.useful_memory_orders_first = policy == "1";
        dag_chain::TimedSearchStats stats;
        dag_chain::Result result;
        double preparation = 0;
        if (string(argv[5]) == "resume") {
            if (argc != 8) throw runtime_error("resume needs a saved plan");
            dag_chain::Result seed;
            seed.solution = load_singleton_plan(g, fs::u8path(argv[7]), cores);
            seed.refined = dag_chain::refine_estimate(g, seed.solution, cores);
            seed.estimate = evaluate_proxy(g, cores, Config{}, seed.solution, 0.03, nullptr);
            seed.generated_for_cores = cores;
            preparation = chrono::duration<double>(Clock::now()-began).count();
            result = dag_chain::continue_from_seed_for_budget(g,cores,Clock::now(),seconds,
                                                              seed,{},&stats,options);
        } else if (string(argv[5]) == "full") {
            if (argc != 7) throw runtime_error("full search does not accept a saved plan");
            result = dag_chain::solve_for_budget(g,cores,began,seconds,&stats,options);
        } else throw runtime_error("unknown mode");
        auto output=fs::u8path(argv[6]);
        if (!output.parent_path().empty()) fs::create_directories(output.parent_path());
        ofstream file(output); file << solution_json(g,result.solution,cores);
        file.close();
        if (!file) throw runtime_error("cannot save probe plan");
        cout << setprecision(15) << "{\"mode\":\"" << argv[5]
             << "\",\"budget_seconds\":" << seconds
             << ",\"preparation_seconds\":" << preparation
             << ",\"total_seconds\":" << chrono::duration<double>(Clock::now()-began).count()
             << ",\"search_seconds\":" << stats.elapsed_seconds
             << ",\"seed_seconds\":" << stats.base_seconds
             << ",\"attempted\":" << stats.attempted
             << ",\"unique\":" << stats.unique_candidates
             << ",\"refined\":" << stats.refined
             << ",\"internal_cycles\":" << result.refined.cycles
             << ",\"internal_bytes\":" << result.refined.route+result.refined.spill
             << ",\"label\":\"" << result.solution.label << "\"}\n";
        return 0;
    } catch(const exception &e) { cerr << e.what() << '\n'; return 1; }
}
