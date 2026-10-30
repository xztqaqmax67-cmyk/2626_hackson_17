#include "memory_grid.hpp"

namespace {
void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

// Independent branches expose multiple ready producers and consumers, with
// unequal footprints competing for UB. No public benchmark input is used.
Graph fixture() {
    constexpr int branches = 24;
    ostringstream text;
    text << "{\"ops\":[";
    for (int v = 0; v < branches * 2; ++v) {
        if (v) text << ',';
        const int branch = v % branches;
        const int cycles = v < branches ? 60 + (branch * 53) % 431 : 90 + (branch * 71) % 631;
        text << "{\"id\":" << v << ",\"op\":\"WORK\",\"pipe\":\""
             << (v < branches ? "PIPE_M" : "PIPE_V") << "\",\"cycles\":" << cycles << '}';
    }
    text << "],\"tensors\":[";
    for (int t = 0; t < branches; ++t) {
        if (t) text << ',';
        text << "{\"id\":" << 1000+t << ",\"pos\":\"UB\",\"size\":"
             << 4096 * (1 + (t * 7) % 24) << '}';
    }
    text << "],\"edges\":[";
    for (int t = 0; t < branches; ++t) {
        if (t) text << ',';
        text << "{\"source\":" << t << ",\"target\":" << 1000+t << "},"
             << "{\"source\":" << 1000+t << ",\"target\":" << branches+t << '}';
    }
    text << "]}";
    return Graph(JsonParser(text.str()).parse());
}

// This is the submitted representation: independent interleavings of different
// core streams are equivalent, so a global order alone is not a plan identity.
vector<vector<int>> streams(const Solution &s, int cores) {
    vector<vector<int>> key(cores);
    for (int v : s.order) key[s.core[v]].push_back(v);
    return key;
}

set<vector<vector<int>>> enumerate(const Graph &g, const vector<int> &assignment,
                                  int cores, bool useful, uint64_t limit) {
    set<vector<vector<int>>> plans;
    for (uint64_t i = 0; i < limit; ++i) {
        const auto s = operation_plan(g, cores, Config{},
            dag_chain::memory_grid::options(i, useful), &assignment, nullptr);
        validate(g, s, cores);
        require(s.core == assignment, "grid changed fixed core ownership");
        plans.insert(streams(s, cores));
    }
    return plans;
}
} // namespace

int main() {
    try {
        // Compare the parameter domain itself, independently of whether one
        // fixture happens to map two different settings to the same plan.
        // Memory pricing cannot influence a window containing one candidate.
        using Setting = tuple<string, int, double, double, double>;
        auto settings = [](bool useful) {
            set<Setting> values;
            for (uint64_t i = 0; i < dag_chain::memory_grid::count(useful); ++i) {
                const auto option = dag_chain::memory_grid::options(i, useful);
                values.emplace(option.mode, option.window,
                    option.window == 1 ? 0.0 : option.memory,
                    option.balance, option.communication);
            }
            return values;
        };
        const auto legacy_settings = settings(false);
        const auto useful_settings = settings(true);
        require(legacy_settings.size() == 132 && useful_settings == legacy_settings,
                "memory grid changed the normalized parameter domain");
        const auto g = fixture();
        for (int cores : {2, 5}) {
            for (int pattern : {0, 1}) {
                vector<int> assignment(g.size());
                for (int v = 0; v < g.size(); ++v)
                    assignment[v] = pattern ? (v % 24) % cores : 0;
                const auto old_all = enumerate(g, assignment, cores, false,
                                              dag_chain::memory_grid::count(false));
                const auto new_all = enumerate(g, assignment, cores, true,
                                              dag_chain::memory_grid::count(true));
                require(new_all == old_all, "reordered grid changed the reachable plan set");
                for (uint64_t mode = 0; mode < 4; ++mode) {
                    const auto first = operation_plan(g, cores, Config{},
                        dag_chain::memory_grid::options(mode, false), &assignment, nullptr);
                    for (uint64_t weight = 1; weight < 8; ++weight) {
                        const auto next = operation_plan(g, cores, Config{},
                            dag_chain::memory_grid::options(weight*4+mode, false), &assignment, nullptr);
                        require(streams(next, cores) == streams(first, cores),
                                "window-one selection unexpectedly depends on memory weight");
                    }
                }
            }
        }
        const vector<int> placement(g.size(), 0);
        const auto old_prefix = enumerate(g, placement, 5, false, 32);
        const auto new_prefix = enumerate(g, placement, 5, true, 32);
        require(old_prefix.size() <= 4, "legacy prefix contains unexpected variation");
        require(new_prefix.size() > old_prefix.size(), "useful prefix did not increase fixture coverage");
        for (bool useful : {false, true}) {
            bool rejected = false;
            try { dag_chain::memory_grid::options(dag_chain::memory_grid::count(useful), useful); }
            catch (const out_of_range &) { rejected = true; }
            require(rejected, "grid accepted out-of-range index");
        }
        cout << "memory grid preserves full plan set; first-32 unique plans "
             << old_prefix.size() << " -> " << new_prefix.size() << '\n';
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
