#pragma once
#include "chain.hpp"

namespace dag_chain {
// 候选数、终止原因和耗时用于观察搜索过程；限时模式的候选集合可能随机器负载变化。
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

// 从本次求解生成的完整种子继续搜索；也供开发暖启动使用，正式程序不读取历史答案。
Result continue_from_seed_for_budget(const Graph &graph, int cores, Clock::time_point started,
                                     double seconds, const Result &seed,
                                     vector<Result> seed_portfolio,
                                     TimedSearchStats *stats = nullptr,
                                     TimedSearchOptions options = {});

// 正式限时入口：种子生成与追加搜索共用预算，只返回完整有效方案。
// started 由 main 在图解析后、求解调用前设置；20 秒不含读图及写出，也不扣收尾预留。
// 种子自身若超预算，须完成后返回；追加搜索的协作检查及收尾也可能造成少量超出。
// 限时模式的候选集合取决于运行进度，输出不保证重复一致。
Result solve_for_budget(const Graph &graph, int cores, Clock::time_point started, double seconds,
                        TimedSearchStats *stats = nullptr, TimedSearchOptions options = {});

// 开发固定工作量对照入口：保留种子赢家，四条独立路线按固定轮数追加；正式入口不调用。
// 相同图与参数的候选、合并顺序一致；计时仅用于开发统计。
Result solve_fixed_work(const Graph &graph, int cores, TimedSearchStats *stats = nullptr);
} // namespace dag_chain
