#pragma once
#include "common.hpp"
namespace problem2 {
// 批量入口只管理总时间与文件，不根据用例名称或内容选择专用算法。
struct BatchOptions {
    Id cores = 4;
    double deadline = 0; // 整批共享的绝对截止时间，包含输入检查和文件导出。
    double total_time_limit = DEFAULT_BATCH_TIME_LIMIT;
    double per_case_limit = 0; // 0 表示在尚未处理的用例间均分剩余时间。
    fs::path output, work;
    std::string search_profile = "compact";
    bool export_results = false;
};
Json run_batch(const fs::path &executable, const fs::path &input_directory, const fs::path &config,
               const BatchOptions &options, const std::function<bool()> &interrupted);
} // namespace problem2
