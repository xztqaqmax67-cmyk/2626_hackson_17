#include "memory_grid.hpp"

namespace {
using Key = tuple<string, int, double>;

void require(bool condition, const string &message) {
    if (!condition) throw runtime_error(message);
}

Key key(const SeedOptions &options) {
    return {options.mode, options.window, options.memory};
}

void check_prefix(uint64_t length, size_t expected_modes, size_t expected_windows,
                  size_t expected_weights) {
    set<string> modes;
    set<int> windows;
    set<double> weights;
    set<pair<string, int>> mode_windows;
    for (uint64_t index = 0; index < length; ++index) {
        const auto option = dag_chain::memory_grid::options(index, true);
        modes.insert(option.mode);
        windows.insert(option.window);
        weights.insert(option.memory);
        mode_windows.emplace(option.mode, option.window);
    }
    require(modes.size() == expected_modes, "prefix lost mode coverage");
    require(windows.size() == expected_windows, "prefix lost ready-window coverage");
    require(weights.size() == expected_weights, "prefix lost memory-weight coverage");
    if (length == 16)
        require(mode_windows.size() == 16, "first 16 options must cover every mode/window pair");
}
} // namespace

int main() {
    try {
        const vector<string> modes{"critical", "dfs", "breadth", "id"};
        const vector<int> windows{4, 8, 16, 32};
        const vector<double> weights{0, 0.125, 0.25, 0.5, 1, 2, 4, 8};
        // 集合来自完整笛卡尔积，而非复制候选枚举公式作为预期答案。
        set<Key> expected;
        for (const auto &mode : modes) {
            for (int window : windows)
                for (double weight : weights)
                    expected.emplace(mode, window, weight);
            expected.emplace(mode, 1, 0);
        }
        require(dag_chain::memory_grid::count(true) == 132 && expected.size() == 132,
                "useful memory-grid size changed");
        set<Key> actual;
        for (uint64_t index = 0; index < 132; ++index) {
            const auto option = dag_chain::memory_grid::options(index, true);
            require(option.balance == 0 && option.communication == 0,
                    "memory-grid experiment changed unrelated placement weights");
            require(actual.insert(key(option)).second, "duplicate useful memory-grid combination");
            require(key(option) == key(dag_chain::memory_grid::options(index, true)),
                    "memory-grid order is not repeatable");
            if (index >= 128)
                require(option.window == 1 && option.memory == 0 && option.mode == modes[index - 128],
                        "the four final mode-only options changed");
        }
        require(actual == expected, "interleaving added or removed a search combination");
        check_prefix(4, 4, 4, 4);
        check_prefix(8, 4, 4, 6);
        check_prefix(16, 4, 4, 8);
        // 验证重复种子按 lane 分片时，每条路线也能很早看到多个窗口和权重。
        for (uint64_t lane = 0; lane < 4; ++lane) {
            set<int> lane_windows;
            set<double> lane_weights;
            for (uint64_t offset = 0; offset < 4; ++offset) {
                const auto option = dag_chain::memory_grid::options(lane + 4 * offset, true);
                lane_windows.insert(option.window);
                lane_weights.insert(option.memory);
            }
            require(lane_windows.size() == 4 && lane_weights.size() == 4,
                    "strided lane prefix lost window/weight diversity");
        }
        // legacy=false 对照网格必须逐项不变，保证消融只影响本次排序。
        const vector<int> legacy_windows{1, 4, 8, 16, 32};
        require(dag_chain::memory_grid::count(false) == 160, "legacy grid size changed");
        for (uint64_t index = 0; index < 160; ++index) {
            const auto option = dag_chain::memory_grid::options(index, false);
            require(key(option) == Key{modes[index % 4], legacy_windows[index / 32],
                                       weights[(index / 4) % 8]},
                    "legacy comparison grid order changed");
        }
        for (bool useful : {false, true}) {
            bool caught = false;
            try { (void)dag_chain::memory_grid::options(dag_chain::memory_grid::count(useful), useful); }
            catch (const out_of_range &) { caught = true; }
            require(caught, "memory-grid upper bound is not checked");
        }
        cout << "memory grid: exact 132-combination set, legacy order, bounds, deterministic prefixes passed; "
                "first 16 cover 4 modes, 4 windows, 8 weights; each lane prefix covers 4 weights\n";
        return 0;
    } catch (const exception &error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
