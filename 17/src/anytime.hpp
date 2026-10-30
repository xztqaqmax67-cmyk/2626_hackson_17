#pragma once
#include "chain.hpp"

namespace dag_chain {
// 候选数和终止原因用于复现搜索过程；耗时字段只作统计，不决定固定搜索结果。
struct TimedSearchStats {
    size_t attempted = 0, unique_candidates = 0, refined = 0, improvements = 0;
    size_t cancelled = 0, workers = 0, exhausted_lanes = 0, cache_limited_lanes = 0;
    double elapsed_seconds = 0, base_seconds = 0;
    bool deadline_reached = false;
};

// 开发实验选项；正式入口只使用默认值，不增加调参 CLI。
struct TimedSearchOptions {
    bool useful_memory_orders_first = true;
};

// 开发工具的暖启动接口；正式程序不会读取已保存的种子或答案。
Result continue_from_seed_for_budget(const Graph &graph, int cores, Clock::time_point started,
                                     double seconds, const Result &seed,
                                     vector<Result> seed_portfolio,
                                     TimedSearchStats *stats = nullptr,
                                     TimedSearchOptions options = {});

// 开发限时入口：先完成基础种子搜索，剩余时间才追加搜索，只返回完整有效方案。
// started 是整个请求的起点，所有工作线程共用这一时间基准。
Result solve_for_budget(const Graph &graph, int cores, Clock::time_point started, double seconds,
                        TimedSearchStats *stats = nullptr, TimedSearchOptions options = {});

// 正式固定工作量入口：保留种子赢家，四条独立路线使用固定轮数追加搜索。
// 相同图与参数的候选、合并顺序一致；计时仅用于开发统计。
Result solve_fixed_work(const Graph &graph, int cores, TimedSearchStats *stats = nullptr);
} // namespace dag_chain
