#pragma once
#include "pipeline.hpp"
namespace problem2 {
NamedPlans activation_plans(const GraphInfo &graph, const Plan &plan, const Settings &settings,
                            double deadline = 0, Id limit = 8);
}
