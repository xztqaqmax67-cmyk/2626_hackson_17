#pragma once
#include "model.hpp"

namespace dag_chain {
// 按张量合并同一来源核心上的消费者，减少重复跨核搬运。
// 邻域结构只构造一次并由工作线程只读共享；实际核心归属来自当前候选，
// 所以搜索继续推进后不会沿用过期分配。所有共享写者始终作为整体迁移。
class TensorNeighborhoods {
    struct Group {
        vector<int> members;
        Row work{};
    };
    vector<Group> groups_;
    vector<int> roots_, tensors_;

  public:
    explicit TensorNeighborhoods(const Graph &g, Deadline *deadline = nullptr);
    // 索引依次编码张量、来源核心、目标核心，以及直接迁移/两种补偿交换。
    // 不满足条件或没有实际移动的槽位返回 false，不发布候选。
    uint64_t count(int cores) const;
    bool propose(const Graph &g, const Solution &base, int cores, uint64_t index,
                 Deadline *deadline, Solution &out) const;
};
} // namespace dag_chain
