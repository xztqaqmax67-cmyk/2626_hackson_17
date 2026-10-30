#pragma once
#include "core.hpp"
namespace problem2 {
struct Candidate {
    std::string kind, label;
    Plan plan;
    bool needs_repair = false;
};
struct SearchOptions {
    // compact 控制原算法已有的探索宽度；正式入口与原命令行默认值保持一致。
    double time_limit = DEFAULT_CASE_TIME_LIMIT, feasibility_grace = 0;
    fs::path warm_start, inherited_checkpoint, resume_checkpoint;
    // no-chain 仅供开发回归对照，正常求解使用 full。
    std::string ablation = "full";
    // Development-only screening experiment; official final acceptance is unchanged.
    std::string screening_proxy = "legacy";
    Id max_evaluations = 0;
    bool compact = false, export_results = true;
};
double pipe_bound(const GraphInfo &graph, const Plan &plan);
// 三类邻域及修复策略全部保留：顺序/切块、分核调整、相关张量组操作。
std::vector<Candidate> neighborhoods(const GraphInfo &graph, const Plan &plan, const Json &profile,
                                     const Json &official_result, int family,
                                     const Settings &settings, bool no_group = false,
                                     bool no_repair = false);
Json solve(const fs::path &graph, Id cores, const fs::path &config, const fs::path &output,
           const SearchOptions &options = {});
// 正式入口：在内存中执行同一套完整搜索，返回经过完整评分的最好方案。
// 不读取配置/历史方案，不写检查点、指标或评分文件；无解时返回 null。
Json solve_in_memory(const GraphInfo &graph, Id cores, const SearchOptions &options = {});
} // namespace problem2
