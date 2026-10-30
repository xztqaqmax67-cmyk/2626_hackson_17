#pragma once
#include "model.hpp"

inline Solution load_singleton_plan(const Graph &g, const fs::path &path, int k) {
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
