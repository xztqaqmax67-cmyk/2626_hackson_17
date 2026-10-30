#pragma once
#include "model.hpp"

namespace dag_chain {
namespace memory_grid {
// ready 窗口只有一个节点时，调整内存权重无法改变选择。
// 先枚举能影响选择的多节点窗口，再各保留一个单节点窗口方案，避免重复工作。
inline uint64_t count(bool useful) { return useful ? 132U : 160U; }

inline SeedOptions options(uint64_t index, bool useful) {
    if (index >= count(useful)) throw out_of_range("memory grid index");
    static const array<string,4> modes{{"critical", "dfs", "breadth", "id"}};
    SeedOptions result;
    result.mode = modes[index % 4U];
    if (!useful) {
        static const array<double,8> weights{{0,0.125,0.25,0.5,1,2,4,8}};
        static const array<int,5> windows{{1,4,8,16,32}};
        result.memory = weights[(index / 4U) % 8U];
        result.window = windows[(index / 32U) % 5U];
    } else if (index < 128U) {
        static const array<int,4> windows{{8,16,32,4}};
        static const array<double,8> weights{{1,2,4,0.5,8,0.25,0.125,0}};
        const uint64_t combination = index / 4U;
        const uint64_t mode = index % 4U;
        const uint64_t window_slot = combination % 4U;
        const uint64_t layer = combination / 4U;
        // 四轮搜索只访问网格前缀：交错模式、窗口与权重，避免前 16 项权重相同。
        // 对固定 mode/window，layer 遍历 0..7 时仍恰好覆盖全部八种权重；
        // 因而这里只改变枚举顺序，保留原 128 个有效组合及末尾四个单节点窗口。
        result.window = windows[(window_slot + mode) % 4U];
        result.memory = weights[(layer + 2U * window_slot + mode) % 8U];
    } else {
        result.window = 1;
        result.memory = 0;
    }
    return result;
}
} // namespace memory_grid
} // namespace dag_chain

