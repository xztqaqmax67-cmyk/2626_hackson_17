#include "chain.hpp"
#include "official_spill.hpp"
#include "spill_feedback.hpp"
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
        if (argc != 4 && argc != 6)
            throw runtime_error("usage: spill_diagnostics graph.json plan.json trace.json [--improve output_plan.json]");
        Graph g(JsonParser(read_text(fs::u8path(argv[1]))).parse());
        const auto plan = JsonParser(read_text(fs::u8path(argv[2]))).parse();
        const auto &mapping = plan.at("node_to_subgraph");
        if (mapping.type != Json::Object || mapping.fields.size() != size_t(g.size()))
            throw runtime_error("diagnostic accepts complete singleton plans only");
        for (const auto &op : g.ops)
            if (mapping.at(to_string(op.id)).integer() != op.id)
                throw runtime_error("diagnostic accepts singleton subgraphs only");
        const int k = int(plan.at("core_schedules").array().size());
        auto s = load_singleton_plan(g, fs::u8path(argv[2]), k);
        if (argc == 6) {
            if (string(argv[4]) != "--improve") throw runtime_error("unknown diagnostic option");
            dag_chain::Result incumbent;
            incumbent.solution = s;
            incumbent.estimate = evaluate_proxy(g, k, Config{}, s, 0.03, nullptr);
            incumbent.refined = dag_chain::refine_estimate(g, s, k);
            incumbent.generated_for_cores = k;
            dag_chain::FeedbackStats statistics;
            auto improved = dag_chain::improve_spill_order(g, k, move(incumbent), &statistics);
            cerr << "feedback_stats " << dag_chain::feedback_stats_json(statistics) << '\n';
            s = move(improved.solution);
            const auto target = fs::u8path(argv[5]);
            if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
            ofstream output(target);
            output << solution_json(g, s, k);
            if (!output) throw runtime_error("cannot write improved plan");
        }
        const auto trace = dag_chain::official_spill_trace(g, s, k);
        const auto target = fs::u8path(argv[3]);
        if (!target.parent_path().empty()) fs::create_directories(target.parent_path());
        ofstream output(target);
        output << dag_chain::official_spill_json(trace);
        if (!output) throw runtime_error("cannot write spill trace");
        if (!trace.valid) { cerr << trace.error << '\n'; return 2; }
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
