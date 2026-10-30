#pragma once
#include "model.hpp"

namespace dag_chain {
namespace memory_grid {
// With one ready candidate the memory price cannot change its selection. Keep
// those four mode-only plans once, after useful multi-candidate windows.
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
        result.window = windows[combination % 4U];
        result.memory = weights[combination / 4U];
    } else {
        result.window = 1;
        result.memory = 0;
    }
    return result;
}
} // namespace memory_grid
} // namespace dag_chain
