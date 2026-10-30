#pragma once
#include "../common.hpp"
namespace problem2 {
Json build_singlecore_plan(const Json &graph);
Json evaluate_singlecore(const Json &graph, double bandwidth, const Capacity &capacity,
                         double cross_wait = 0, double same_wait = 0);
} // namespace problem2
