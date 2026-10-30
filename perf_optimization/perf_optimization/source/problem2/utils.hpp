#pragma once
#include "common.hpp"
namespace problem2 {
std::string read_text(const fs::path &path);
void write_text(const fs::path &path, const std::string &text);
Json read_json(const fs::path &path);
Json read_json_strict(const fs::path &path);
void atomic_write(const fs::path &path, const Json &value);
void atomic_write(const fs::path &path, const std::string &text);
std::string sha256(const fs::path &path);
std::string sha256_text(const std::string &text);
} // namespace problem2
