#include "tensor_neighborhood.hpp"

namespace dag_chain {
namespace {
void check(Deadline *deadline) { if (deadline) deadline->check(); }
constexpr size_t tensor_limit = 24;
}

TensorNeighborhoods::TensorNeighborhoods(const Graph &g, Deadline *deadline)
    : groups_(g.size()) {
    check(deadline);
    for (int v : g.topo) {
        if ((v & 127) == 0) check(deadline);
        auto &group = groups_[g.writer_group[v]];
        if (group.members.empty()) roots_.push_back(g.writer_group[v]);
        group.members.push_back(v);
        group.work[g.ops[v].pipe] += g.weight(v);
    }
    // Static priority only chooses which tensors to explore; legality and final
    // selection remain the existing complete-graph evaluator's responsibility.
    vector<pair<double, int>> priority;
    for (size_t t = 0; t < g.tensors.size(); ++t) {
        if ((t & 127U) == 0) check(deadline);
        const auto &tensor = g.tensors[t];
        if (tensor.consumers.size() < 2) continue;
        priority.emplace_back(-double(tensor.size) * double(tensor.consumers.size()), int(t));
    }
    const size_t keep = min(tensor_limit, priority.size());
    partial_sort(priority.begin(), priority.begin() + keep, priority.end());
    for (size_t i = 0; i < keep; ++i) tensors_.push_back(priority[i].second);
    check(deadline);
}

uint64_t TensorNeighborhoods::count(int cores) const {
    if (cores < 1 || cores > 5) throw runtime_error("tensor neighborhood core count must be 1..5");
    return uint64_t(tensors_.size()) * uint64_t(cores) * uint64_t(cores) * 3U;
}

bool TensorNeighborhoods::propose(const Graph &g, const Solution &base, int cores,
                                  uint64_t index, Deadline *deadline, Solution &out) const {
    check(deadline);
    if (index >= count(cores)) return false;
    if (base.core.size() != size_t(g.size()) || groups_.size() != size_t(g.size()))
        throw runtime_error("tensor neighborhood graph/solution size mismatch");
    const int variant = int(index % 3U); index /= 3U;
    const int target = int(index % uint64_t(cores)); index /= uint64_t(cores);
    const int source = int(index % uint64_t(cores)); index /= uint64_t(cores);
    if (source == target) return false;
    const int tid = tensors_[size_t(index)];
    const auto &tensor = g.tensors[tid];

    vector<unsigned char> producer(g.size()), involved(g.size()), moving(g.size());
    bool target_uses = false;
    size_t visited = 0;
    for (int v : tensor.producers) {
        if ((visited++ & 127U) == 0) check(deadline);
        producer[g.writer_group[v]] = involved[g.writer_group[v]] = 1;
        target_uses = target_uses || base.core[v] == target;
    }
    vector<int> selected;
    Row work{};
    for (int v : tensor.consumers) {
        if ((visited++ & 127U) == 0) check(deadline);
        const int root = g.writer_group[v];
        involved[root] = 1;
        target_uses = target_uses || base.core[v] == target;
        // A consumer may share a writer binding with this tensor's producer.
        // That entire group stays fixed instead of silently dragging it along.
        if (base.core[v] != source || producer[root] || moving[root]) continue;
        moving[root] = 1;
        selected.push_back(root);
        for (int p = 0; p < 5; ++p) work[p] += groups_[root].work[p];
    }
    if (selected.empty() || !target_uses) return false;
    check(deadline);

    int compensate = -1;
    if (variant) {
        // Keep only two candidates, avoiding an O(groups) temporary candidate
        // array. Match all five pipeline loads, rather than total work alone.
        array<pair<double, int>, 2> closest{{{INF, -1}, {INF, -1}}};
        for (size_t i = 0; i < roots_.size(); ++i) {
            if ((i & 127U) == 0) check(deadline);
            const int root = roots_[i];
            const auto &group = groups_[root];
            if (involved[root] || base.core[group.members.front()] != target) continue;
            double mismatch = 0;
            for (int p = 0; p < 5; ++p)
                mismatch += abs(group.work[p] - work[p]);
            pair<double, int> key{mismatch, root};
            if (key < closest[0]) { closest[1] = closest[0]; closest[0] = key; }
            else if (key < closest[1]) closest[1] = key;
        }
        compensate = closest[size_t(variant - 1)].second;
        if (compensate < 0) return false;
    }

    Solution candidate = base;
    size_t changed = 0;
    for (int root : selected)
        for (int v : groups_[root].members) {
            if ((changed++ & 127U) == 0) check(deadline);
            candidate.core[v] = target;
        }
    if (compensate >= 0)
        for (int v : groups_[compensate].members) {
            if ((changed++ & 127U) == 0) check(deadline);
            candidate.core[v] = source;
        }
    candidate.label = "tensor_consumers_" + to_string(tid) + "_" + to_string(source) +
                      "_" + to_string(target) + "_" + to_string(variant);
    validate(g, candidate, cores);
    check(deadline);
    // Only publish a completely constructed candidate; cancellation leaves out
    // and the parent untouched, as in the existing anytime search.
    out = move(candidate);
    return true;
}
} // namespace dag_chain
