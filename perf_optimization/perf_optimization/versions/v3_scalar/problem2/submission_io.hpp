#pragma once
#include "common.hpp"
namespace problem2 {
std::string safe_message(const std::string &message);
// 正式提交接口与开发接口分开解析，避免开发参数进入验收命令行。
struct SubmissionArguments {
    fs::path input, output;
    Id cores = 4;
    bool version = false, help = false;
};
struct SubmissionUsageError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};
SubmissionArguments parse_submission_arguments(const std::vector<std::string> &arguments);
} // namespace problem2
