#pragma once
#include "../common.hpp"
namespace problem2 {
struct EvaluationValidationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void require_integer(const Json &value, const std::string &name, Id minimum = 0);
void require_number(const Json &value, const std::string &name, bool positive = false);
void validate_capacity(const Json &capacity);
Capacity read_integer_settings(const fs::path &path, const std::string &section,
                               const std::set<std::string> &keys);
Capacity read_required_settings(const fs::path &path, const std::string &section,
                                const std::set<std::string> &keys);
Capacity read_capacity_config(const fs::path &path);
Id read_bandwidth_config(const fs::path &path);
Settings read_evaluation_config(const fs::path &path);
void validate_parameters(double bandwidth, const Capacity &capacity, Id max_iter);
void check_acyclic(const Ids &nodes, const std::vector<std::tuple<Id, Id, std::string>> &edges,
                   const std::string &context);
void validate_graph(const Json &graph);
void validate_task_order(const Json &view);
} // namespace problem2
