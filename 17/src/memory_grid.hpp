#pragma once
#include "model.hpp"

namespace dag_chain {
namespace memory_grid {
// 正式搜索使用 132 个组合：4 种排序模式 × 4 种多节点窗口 × 8 种内存权重，
// 再加 4 个单节点窗口。窗口只有一个就绪节点时，权重不影响选择，无需重复枚举。
// useful=false 保留开发对照所用的 160 项原始网格。
inline uint64_t count(bool useful) { return useful ? 132U : 160U; }

inline SeedOptions options(uint64_t index, bool useful) {
    if (index >= count(useful))
        throw out_of_range("memory grid index");
    static const array<string, 4> modes{{"critical", "dfs", "breadth", "id"}};
    SeedOptions result;
    result.mode = modes[index % 4U];
    if (!useful) {
        static const array<double, 8> weights{{0, 0.125, 0.25, 0.5, 1, 2, 4, 8}};
        static const array<int, 5> windows{{1, 4, 8, 16, 32}};
        result.memory = weights[(index / 4U) % 8U];
        result.window = windows[(index / 32U) % 5U];
    } else if (index < 128U) {
        static const array<int, 4> windows{{8, 16, 32, 4}};
        static const array<double, 8> weights{{1, 2, 4, 0.5, 8, 0.25, 0.125, 0}};
        const uint64_t combination = index / 4U;
        const uint64_t mode = index % 4U;
        const uint64_t window_slot = combination % 4U;
        const uint64_t layer = combination / 4U;
        // 固定搜索通常只能访问部分网格。交错模式、窗口和权重，使前 16 项
        // 就覆盖全部模式与窗口的配对，并出现全部 8 种权重。
        // 固定 mode/window 后，layer 遍历 0..7 仍恰好覆盖每种权重一次，
        // 因而交错只改变尝试顺序，不删减或重复这 128 个组合。
        result.window = windows[(window_slot + mode) % 4U];
        result.memory = weights[(layer + 2U * window_slot + mode) % 8U];
    } else {
        // 每种排序模式只保留一个单节点窗口，内存权重统一取 0。
        result.window = 1;
        result.memory = 0;
    }
    return result;
}
} // namespace memory_grid
} // namespace dag_chain
