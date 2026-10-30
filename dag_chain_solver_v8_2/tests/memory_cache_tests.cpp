#include "model.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

// Frozen pre-optimization MemoryOrder formulas. This differential oracle checks
// values and every state transition, including accumulation order, rather than
// merely checking that the new caches contain unique values.
struct LegacyMemoryOrder {
    const Graph &g;
    const Config &cfg;
    int k;
    vector<int> remaining;
    vector<unsigned char> live;
    vector<array<double, 2>> used;
    LegacyMemoryOrder(const Graph &graph, const Config &config, int cores, const vector<int> &a)
        : g(graph), cfg(config), k(cores), remaining(g.tensors.size() * size_t(k)),
          live(remaining.size()), used(k) {
        for (int t = 0; t < int(g.tensors.size()); ++t) {
            auto users = g.tensors[t].consumers;
            users.insert(users.end(), g.tensors[t].producers.begin(), g.tensors[t].producers.end());
            Graph::unique_vec(users);
            for (int v : users) ++remaining[size_t(t) * k + a[v]];
        }
    }
    vector<int> operands(int v) const {
        auto all = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(all.begin(), all.end(), t) == all.end()) all.push_back(t);
        return all;
    }
    double price(int v, int c) const {
        array<double, 2> add{}, release{};
        const auto all = operands(v);
        for (int t : all)
            if (!live[size_t(t) * k + c]) add[g.tensors[t].pos] += double(g.tensors[t].size);
        for (int t : all)
            if (remaining[size_t(t) * k + c] == 1)
                release[g.tensors[t].pos] += double(g.tensors[t].size);
        double value = 0;
        for (int m = 0; m < 2; ++m) {
            double limit = double(cfg.spill_limit(m));
            value += add[m] - release[m] + 4 * max(0.0, used[c][m] + add[m] - limit);
        }
        return value / cfg.bandwidth;
    }
    void commit(int v, int c) {
        const auto all = operands(v);
        for (int t : all) {
            size_t index = size_t(t) * k + c;
            if (!live[index]) {
                live[index] = 1;
                used[c][g.tensors[t].pos] += double(g.tensors[t].size);
            }
        }
        for (int t : all) --remaining[size_t(t) * k + c];
        for (int t : all) {
            size_t index = size_t(t) * k + c;
            if (!remaining[index] && live[index]) {
                live[index] = 0;
                used[c][g.tensors[t].pos] -= double(g.tensors[t].size);
            }
        }
    }
};

Graph branching_fixture(int edge_style, bool large_sizes) {
    const vector<int> tensor_ids{100, 103, 101, 106, 102, 104, 105, 107};
    const vector<i64> sizes = large_sizes ?
        vector<i64>{9007199254740993LL, 1, 31, 7, 2, 5, 9, 0} :
        vector<i64>{80000, 13000, 47000, 17000, 61000, 0, 9000, 0};
    vector<pair<int, int>> edges{
        {103, 0}, {0, 102}, {100, 0}, {0, 101}, {103, 1}, {1, 101},
        {100, 2}, {2, 104}, {102, 3}, {3, 105}, {104, 4}, {101, 5},
        {5, 106}, {106, 6}, {104, 6}, {105, 6}};
    if (edge_style == 1) reverse(edges.begin(), edges.end());
    if (edge_style == 2) rotate(edges.begin(), edges.begin() + 5, edges.end());
    ostringstream text;
    text << "{\"ops\":[";
    for (int v = 0; v < 8; ++v) {
        if (v) text << ',';
        text << "{\"id\":" << v
             << ",\"op\":\"WORK\",\"pipe\":\"PIPE_M\",\"cycles\":1}";
    }
    text << "],\"tensors\":[";
    for (size_t t = 0; t < tensor_ids.size(); ++t) {
        if (t) text << ',';
        text << "{\"id\":" << tensor_ids[t] << ",\"size\":" << sizes[t]
             << ",\"pos\":\"" << (t % 3 == 0 ? "L1" : t % 3 == 1 ? "UB" : "DDR") << "\"}";
    }
    text << "],\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
        if (i) text << ',';
        text << "{\"source\":" << edges[i].first << ",\"target\":" << edges[i].second << '}';
    }
    text << "]}";
    return Graph(JsonParser(text.str()).parse());
}

vector<int> ready_order(const Graph &g, int shift) {
    vector<int> degree(g.size()), ready, result;
    for (int v = 0; v < g.size(); ++v) {
        degree[v] = int(g.ops[v].pred.size());
        if (!degree[v]) ready.push_back(v);
    }
    while (!ready.empty()) {
        auto chosen = min_element(ready.begin(), ready.end(), [&](int a, int b) {
            return (a + shift) % g.size() < (b + shift) % g.size();
        });
        int v = *chosen;
        ready.erase(chosen);
        result.push_back(v);
        for (int w : g.ops[v].succ) if (!--degree[w]) ready.push_back(w);
    }
    return result;
}

void verify(const Graph &g) {
    require(g.op_tensor_uses.size() == size_t(g.size()) &&
                g.op_tensor_uses_sorted.size() == size_t(g.size()) &&
                g.tensor_users.size() == g.tensors.size(), "cache sizes differ from graph");
    for (int v = 0; v < g.size(); ++v) {
        auto expected = g.ops[v].inputs;
        for (int t : g.ops[v].outputs)
            if (find(expected.begin(), expected.end(), t) == expected.end()) expected.push_back(t);
        require(g.op_tensor_uses[v] == expected, "input/output insertion order changed");
        Graph::unique_vec(expected);
        require(g.op_tensor_uses_sorted[v] == expected, "sorted traffic/spill use order changed");
    }
    for (size_t t = 0; t < g.tensors.size(); ++t) {
        auto users = g.tensors[t].consumers;
        users.insert(users.end(), g.tensors[t].producers.begin(), g.tensors[t].producers.end());
        Graph::unique_vec(users);
        require(g.tensor_users[t] == users, "writer/consumer user set changed");
    }
    Config cfg;
    for (int k = 2; k <= 5; ++k)
        for (int pattern = 0; pattern < 3; ++pattern) {
            vector<int> placement(g.size());
            for (int v = 0; v < g.size(); ++v) placement[v] = pattern ? (v * pattern + 1) % k : 0;
            placement = g.normalize_writers(move(placement));
            for (int shift = 0; shift < max(1, g.size()); ++shift) {
                MemoryOrder cached(g, cfg, k, placement);
                LegacyMemoryOrder legacy(g, cfg, k, placement);
                auto same_state = [&]() {
                    require(cached.remaining == legacy.remaining && cached.live == legacy.live &&
                                cached.used == legacy.used, "MemoryOrder state changed");
                };
                same_state();
                for (int v : ready_order(g, shift)) {
                    for (int w = 0; w < g.size(); ++w)
                        for (int c = 0; c < k; ++c)
                            require(cached.price(w, c) == legacy.price(w, c),
                                    "MemoryOrder price changed (including floating accumulation order)");
                    same_state(); // Repeated pricing is read-only.
                    cached.commit(v, placement[v]);
                    legacy.commit(v, placement[v]);
                    same_state();
                }
            }
        }
}
} // namespace

int main() {
    try {
        verify(Graph(JsonParser(R"({"ops":[],"tensors":[],"edges":[]})").parse()));
        verify(Graph(JsonParser(R"({"ops":[{"id":1,"op":"COPY_IN","pipe":"PIPE_MTE2","cycles":-1},{"id":2,"op":"COPY_OUT","pipe":"PIPE_MTE3","cycles":-1}],"tensors":[{"id":100,"pos":"L1","size":128},{"id":101,"pos":"DDR","size":128},{"id":102,"pos":"DDR","size":128}],"edges":[{"source":101,"target":1},{"source":1,"target":100},{"source":100,"target":2},{"source":2,"target":102}]})").parse()));
        for (int style = 0; style < 3; ++style)
            for (bool large : {false, true}) verify(branching_fixture(style, large));
        // An Op reading and writing the same logical Tensor forms a cycle in
        // the accepted bipartite input model; caching must not broaden that contract.
        bool cycle_rejected = false;
        try {
            Graph invalid(JsonParser(R"({"ops":[{"id":1,"op":"WORK","pipe":"PIPE_M","cycles":1}],"tensors":[{"id":100,"pos":"UB","size":8}],"edges":[{"source":1,"target":100},{"source":100,"target":1}]})").parse());
        } catch (const runtime_error &) { cycle_rejected = true; }
        require(cycle_rejected, "input/output cycle was unexpectedly accepted");
        cout << "cached Tensor uses preserve legacy prices, states and traversal order\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}

