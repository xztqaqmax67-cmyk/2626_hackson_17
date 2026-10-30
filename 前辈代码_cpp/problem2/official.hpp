#pragma once
#include "core.hpp"
namespace problem2::official {
Json load_graph(const fs::path &path);
Settings load_settings(const fs::path &path);
Json build_boundary_graphs(const Json &graph, const Json &plan, double bandwidth = 1);
Json route_cost(const Json &graph, const Json &plan);
Json build_profile(const Json &graph, const Json &plan, const Settings &settings,
                   const Json &result = Json());
Json evaluate(const Json &graph, const Json &plan, const Settings &settings, bool profile = false,
              double deadline = 0);
} // namespace problem2::official
