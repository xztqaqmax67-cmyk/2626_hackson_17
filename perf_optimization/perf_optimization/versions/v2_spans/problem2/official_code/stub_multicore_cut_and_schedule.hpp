#pragma once
#include "../common.hpp"
namespace problem2 {
struct MulticoreCutError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
std::pair<Adjacency, Adjacency> contract_excluded_copy_nodes(const Ids &eligible,
                                                             const Adjacency &successors);
Json derive_multicore_plan(const Json &graph, const Json &plan);
bool validate_multicore_plan(const Json &graph, const Json &plan);
Json generate_multicore_plan(const Json &graph, Id cores = 4, std::uint64_t seed = 0,
                             Id min_size = 50, Id max_size = 100);
} // namespace problem2
