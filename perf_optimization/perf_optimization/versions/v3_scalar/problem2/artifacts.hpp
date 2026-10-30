#pragma once
#include "common.hpp"
namespace problem2 {
std::string export_evaluation(const fs::path &directory, const std::string &input,
                              const std::string &plan, const Json &result);
Json recover(const fs::path &directory, const std::string &case_name, Id cores);
} // namespace problem2
