#pragma once
#include "chain.hpp"

namespace dag_chain {
struct TimedSearchStats {
    size_t attempted = 0, unique_candidates = 0, refined = 0, improvements = 0;
    size_t cancelled = 0, workers = 0, exhausted_lanes = 0, cache_limited_lanes = 0;
    double elapsed_seconds = 0, base_seconds = 0;
    bool deadline_reached = false;
};

// 先完整取得 v5 種子，剩餘時間才搜尋；只提交完整且有效的候選。
// started 是整個請求的起點，不是每個 worker 各自的計時起點。
Result solve_for_budget(const Graph &graph, int cores, Clock::time_point started,
                        double seconds, TimedSearchStats *stats = nullptr);
} // namespace dag_chain
