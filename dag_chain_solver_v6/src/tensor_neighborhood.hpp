#pragma once
#include "model.hpp"

namespace dag_chain {
// Consumer-side moves adapted from the predecessor's tensor-group neighborhood.
// Build once, then share read-only across workers. The assignment is always
// taken from the current parent, so groups track the evolving search solution.
class TensorNeighborhoods {
    struct Group { vector<int> members; Row work{}; };
    vector<Group> groups_;
    vector<int> roots_, tensors_;
public:
    explicit TensorNeighborhoods(const Graph &g, Deadline *deadline = nullptr);
    // Index order: tensor, source core, destination core, then plain move / the
    // two closest compensating writer groups. Invalid/no-op slots return false.
    uint64_t count(int cores) const;
    bool propose(const Graph &g, const Solution &base, int cores, uint64_t index,
                 Deadline *deadline, Solution &out) const;
};
} // namespace dag_chain
