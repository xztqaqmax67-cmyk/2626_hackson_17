#pragma once
#include "../utils.hpp"
namespace problem2 {
std::string format_data_movement_log(const Json &result);
std::string format_multicore_result_log(const std::string &filename, const Json &result);
std::string format_trace_json(const std::string &filename, const Json &result);
int run_problem_cli(int argc, char **argv);
int run_multicore_stub_cli(int argc, char **argv);
} // namespace problem2
