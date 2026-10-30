#include "tensor_neighborhood.hpp"

namespace dag_chain {
namespace {
void check(Deadline *deadline) {
    if (deadline)
        deadline->check();
}
// 只为优先级最高的少量张量构造邻域，限制大图的追加搜索成本。
constexpr size_t tensor_limit = 24;
} // namespace

TensorNeighborhoods::TensorNeighborhoods(const Graph &g, Deadline *deadline) : groups_(g.size()) {
    check(deadline);
    // writer_group 已包含共享写者的传递闭包；成员必须整体迁移以维持同核约束。
    for (int v : g.topo) {
        if ((v & 127) == 0)
            check(deadline);
        auto &group = groups_[g.writer_group[v]];
        if (group.members.empty())
            roots_.push_back(g.writer_group[v]);
        group.members.push_back(v);
        group.work[g.ops[v].pipe] += g.weight(v);
    }
    // 按张量大小与消费者数量排序，优先探索可能造成重复搬运的张量。
    // 这是候选生成顺序；候选仍须经过合法性检查及全图估价。
    vector<pair<double, int>> priority;
    for (size_t t = 0; t < g.tensors.size(); ++t) {
        if ((t & 127U) == 0)
            check(deadline);
        const auto &tensor = g.tensors[t];
        if (tensor.consumers.size() < 2)
            continue;
        priority.emplace_back(-double(tensor.size) * double(tensor.consumers.size()), int(t));
    }
    const size_t keep = min(tensor_limit, priority.size());
    partial_sort(priority.begin(), priority.begin() + keep, priority.end());
    for (size_t i = 0; i < keep; ++i)
        tensors_.push_back(priority[i].second);
    check(deadline);
}

uint64_t TensorNeighborhoods::count(int cores) const {
    if (cores < 1 || cores > 5)
        throw runtime_error("tensor neighborhood core count must be 1..5");
    return uint64_t(tensors_.size()) * uint64_t(cores) * uint64_t(cores) * 3U;
}

bool TensorNeighborhoods::propose(const Graph &g, const Solution &base, int cores, uint64_t index,
                                  Deadline *deadline, Solution &out) const {
    check(deadline);
    if (index >= count(cores))
        return false;
    if (base.core.size() != size_t(g.size()) || groups_.size() != size_t(g.size()))
        throw runtime_error("tensor neighborhood graph/solution size mismatch");
    // 同一张量和源/目标核心尝试三种变体：直接迁移，或交换两种补偿组之一。
    const int variant = int(index % 3U);
    index /= 3U;
    const int target = int(index % uint64_t(cores));
    index /= uint64_t(cores);
    const int source = int(index % uint64_t(cores));
    index /= uint64_t(cores);
    if (source == target)
        return false;
    const int tid = tensors_[size_t(index)];
    const auto &tensor = g.tensors[tid];

    vector<unsigned char> producer(g.size()), involved(g.size()), moving(g.size());
    bool target_uses = false;
    size_t visited = 0;
    for (int v : tensor.producers) {
        if ((visited++ & 127U) == 0)
            check(deadline);
        producer[g.writer_group[v]] = involved[g.writer_group[v]] = 1;
        target_uses = target_uses || base.core[v] == target;
    }
    vector<int> selected;
    Row work{};
    for (int v : tensor.consumers) {
        if ((visited++ & 127U) == 0)
            check(deadline);
        const int root = g.writer_group[v];
        involved[root] = 1;
        target_uses = target_uses || base.core[v] == target;
        // 若消费者与该张量的生产者属于同一写者闭包，整个组保持原位，
        // 避免聚合消费者时连带迁移生产者；同一闭包最多加入一次。
        if (base.core[v] != source || producer[root] || moving[root])
            continue;
        moving[root] = 1;
        selected.push_back(root);
        for (int p = 0; p < 5; ++p)
            work[p] += groups_[root].work[p];
    }
    // 目标核心已使用该张量时，聚合消费者才有机会减少跨核复制。
    if (selected.empty() || !target_uses)
        return false;
    check(deadline);

    int compensate = -1;
    if (variant) {
        // 从目标核心选一个与当前张量无关的写者组换回源核心，补偿迁移负载。
        // 按五条流水线的工作量差异匹配；只保留最接近的两组，节省临时存储。
        array<pair<double, int>, 2> closest{{{INF, -1}, {INF, -1}}};
        for (size_t i = 0; i < roots_.size(); ++i) {
            if ((i & 127U) == 0)
                check(deadline);
            const int root = roots_[i];
            const auto &group = groups_[root];
            if (involved[root] || base.core[group.members.front()] != target)
                continue;
            double mismatch = 0;
            for (int p = 0; p < 5; ++p)
                mismatch += abs(group.work[p] - work[p]);
            pair<double, int> key{mismatch, root};
            if (key < closest[0]) {
                closest[1] = closest[0];
                closest[0] = key;
            } else if (key < closest[1])
                closest[1] = key;
        }
        compensate = closest[size_t(variant - 1)].second;
        if (compensate < 0)
            return false;
    }

    Solution candidate = base;
    size_t changed = 0;
    for (int root : selected)
        for (int v : groups_[root].members) {
            if ((changed++ & 127U) == 0)
                check(deadline);
            candidate.core[v] = target;
        }
    if (compensate >= 0)
        for (int v : groups_[compensate].members) {
            if ((changed++ & 127U) == 0)
                check(deadline);
            candidate.core[v] = source;
        }
    candidate.label = "tensor_consumers_" + to_string(tid) + "_" + to_string(source) + "_" +
                      to_string(target) + "_" + to_string(variant);
    validate(g, candidate, cores);
    check(deadline);
    // 完整构造并校验成功后才发布候选；限时模式取消时不修改 out 或父方案。
    out = move(candidate);
    return true;
}
} // namespace dag_chain
