#pragma once
#include "../common.hpp"
namespace problem2 {
struct Step2SchedulingError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
using TensorUses = std::map<Id, std::vector<std::pair<Id, Id>>>;
TensorUses build_tensor_uses(const Json &graph, const Ids &sequence);
std::map<Id, Id> find_copy_in_backings(const Json &graph);
Json step2_spill_insertion(const Json &graph, const Ids &sequence, const Capacity &capacity);
Json build_extended_graph(const Json &graph, const Json &result);
Json simulate_occupancy(const Json &graph);
} // namespace problem2
