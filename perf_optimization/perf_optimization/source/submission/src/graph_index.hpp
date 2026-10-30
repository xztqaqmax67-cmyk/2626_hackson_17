#pragma once
#include "common.hpp"
namespace problem2 {
void validate_graph(const Json &graph);
std::pair<Adjacency, Adjacency> contract_excluded_copy_nodes(const Ids &ids, const Adjacency &succs);
}
