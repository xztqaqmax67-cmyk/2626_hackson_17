#pragma once
#include "../common.hpp"
namespace problem2 {
std::map<Id, Id> precompute_depth(const Adjacency &preds, const Adjacency &succs, const Ids &nodes);
Ids step1_from_adj(const Adjacency &preds, const Adjacency &succs, const Ids &nodes,
                   const Json &ops = Json::array());
Ids step1_schedule(const Json &graph);
bool check_topo(const Json &graph, const Ids &sequence);
} // namespace problem2
