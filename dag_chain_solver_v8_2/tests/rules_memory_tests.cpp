#include "chain.hpp"

namespace {
void require(bool ok, const string &message) {
    if (!ok)
        throw runtime_error(message);
}

// Tiny logical graphs with fixed order; no official scheduling/evaluation code.
Graph graph(int count, const vector<i64> &sizes, const vector<vector<int>> &writers,
            const vector<vector<int>> &readers, const string &memory = "UB") {
    ostringstream text;
    text << "{\"ops\":[";
    for (int v = 0; v < count; ++v) {
        if (v) text << ',';
        text << "{\"id\":" << v
             << ",\"op\":\"CUSTOM\",\"pipe\":\"PIPE_V\",\"cycles\":1}";
    }
    text << "],\"tensors\":[";
    for (size_t t = 0; t < sizes.size(); ++t) {
        if (t) text << ',';
        text << "{\"id\":" << 100 + t << ",\"pos\":\"" << memory
             << "\",\"size\":" << sizes[t] << '}';
    }
    text << "],\"edges\":[";
    bool first = true;
    auto edge = [&](size_t source, size_t target) {
        if (!first) text << ',';
        first = false;
        text << "{\"source\":" << source << ",\"target\":" << target << '}';
    };
    for (size_t t = 0; t < sizes.size(); ++t) {
        for (int v : writers[t]) edge(size_t(v), 100 + t);
        for (int v : readers[t]) edge(100 + t, size_t(v));
    }
    text << "]}";
    return Graph(JsonParser(text.str()).parse());
}

SpillEstimate estimate(const Graph &g, const Config &cfg) {
    Solution s;
    s.core.assign(g.size(), 0);
    s.order.resize(g.size());
    iota(s.order.begin(), s.order.end(), 0);
    validate(g, s, 2);
    return estimate_spills(g, cfg, s, 2, nullptr);
}
} // namespace

int main() {
    try {
        Config cfg;
        require(cfg.capacity == array<i64, 2>{1572864, 131072} &&
                    cfg.bandwidth == 60 && cfg.delay == 1000,
                "fixed parameters must follow supplied config and submission contract");
        require(cfg.spill_limit(0) == 1415577 && cfg.spill_limit(1) == 117964,
                "soft limits must floor 90 percent of each hard capacity");
        cfg.capacity = {100, 100};
        for (const string &memory : {string("L1"), string("UB")}) {
            auto soft = estimate(graph(4, {60, 35}, {{0}, {1}}, {{3}, {2}}, memory), cfg);
            require(!soft.infeasible && soft.bytes == 120,
                    "95-byte live set must spill an idle 60-byte tensor at the 90-byte limit");
            auto transient = estimate(graph(2, {95}, {{0}}, {{1}}, memory), cfg);
            require(!transient.infeasible && transient.bytes == 0 &&
                        transient.peak[memory == "L1" ? 0 : 1] == 95,
                    "protected footprint above soft limit but within hard capacity is allowed");
            auto idle = estimate(graph(3, {95}, {{0}}, {{2}}, memory), cfg);
            require(!idle.infeasible && idle.bytes == 190,
                    "an op with no allocation must still spill idle data above the soft limit");
            require(estimate(graph(2, {101}, {{0}}, {{1}}, memory), cfg).infeasible,
                    "one tensor above the hard limit cannot fit");
            auto hard = estimate(graph(3, {60, 60}, {{0}, {1}}, {{1}, {2}}, memory), cfg);
            require(hard.infeasible,
                    "last input and new output must coexist before last-use release");
        }
        auto partial = estimate(graph(7, {60, 35, 35}, {{0, 3}, {1}, {4}},
                                      {{6}, {2}, {5}}), cfg);
        require(!partial.infeasible && partial.partial_write_victims == vector<int>{0},
                "partial-write eviction is legal and remains an optional ordering hint");
        require(partial.bytes == 240,
                "a later write invalidates backing: two writebacks and two reloads are required");
        auto clean_after_write = estimate(graph(7, {60, 35, 35}, {{0}, {1}, {4}},
                                                {{3, 6}, {2}, {5}}), cfg);
        require(!clean_after_write.infeasible && clean_after_write.bytes == 180,
                "a reloaded unmodified output reuses its backing on the second eviction");
        auto clean = estimate(graph(7, {60, 35, 35}, {{}, {1}, {4}},
                                    {{0, 3, 6}, {2}, {5}}), cfg);
        require(!clean.infeasible && clean.bytes == 120,
                "unchanged input backing permits two reloads without redundant writeback");
        auto tied = estimate(graph(4, {70, 20, 10}, {{0}, {0}, {1}},
                                   {{3}, {3}, {2}}), cfg);
        require(!tied.infeasible && tied.bytes == 140,
                "equal next uses must evict the oldest active allocation first");
        cout << "new rules: soft/hard limits, allocation peak, partial writes and dirty backing passed\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}

