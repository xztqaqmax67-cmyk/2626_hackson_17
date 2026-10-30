#pragma once
#include "common.hpp"
namespace problem2 {
// 原程序遍历 set(tensor_ids) 的次序会影响空闲内存额度的复用顺序。
// std::set 按数值排序，std::unordered_set 的顺序也不同，均不能直接替换。
// 此处只模拟 CPython 64 位整数集合的插入/扩容；不包含解释器或 Python 调用。
// 算法依据：CPython Objects/setobject.c，LINEAR_PROBES=9、PERTURB_SHIFT=5。
inline Ids python_set_order(const Ids &values) {
    std::vector<std::optional<Id>> table(8);
    std::size_t used = 0;
    auto insert = [](auto &slots, Id value) {
        std::size_t mask = slots.size() - 1;
        // 非负 Python 整数的哈希模数为 2^61-1。
        std::uint64_t hash = std::uint64_t(value) % 2305843009213693951ULL, perturb = hash;
        std::size_t index = hash & mask;
        while (true) {
            std::size_t probes = index + 9 <= mask ? 9 : 0;
            for (std::size_t offset = 0; offset <= probes; ++offset) {
                auto &slot = slots[index + offset];
                if (!slot) {
                    slot = value;
                    return true;
                }
                if (*slot == value) {
                    return false;
                }
            }
            perturb >>= 5;
            index = (index * 5 + 1 + perturb) & mask;
        }
    };
    for (Id value : values) {
        if (!insert(table, value)) {
            continue;
        }
        ++used;
        if (used * 5 < (table.size() - 1) * 3) {
            continue;
        }
        std::size_t minimum = used * (used > 50000 ? 2 : 4), size = 8;
        while (size <= minimum) {
            size *= 2;
        }
        auto previous = std::move(table);
        table.resize(size);
        for (auto slot : previous) {
            if (slot) {
                insert(table, *slot);
            }
        }
    }
    Ids result;
    for (auto slot : table) {
        if (slot) {
            result.push_back(*slot);
        }
    }
    return result;
}
} // namespace problem2
