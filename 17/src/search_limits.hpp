#pragma once
#include <cstddef>

namespace dag_chain {
namespace search_limits {
// 调整搜索工作量的参数，修改后须重新编译；正式 CLI 不提供调参选项。
// 跨核延迟、DDR 带宽及 L1/UB 容量定义在 Config 中，属于固定题目规则。

// 正式程序使用固定工作量搜索；限时模式仅供开发测试。
// 关闭限时模式时，total_seconds 不参与正式求解的候选选择。
inline constexpr bool enable_timed_search = false;

// 固定工作量追加搜索：每条路线每轮最多粗估 12 次、精评 3 次。
// 小图 4 轮、中图 2 轮、超大图不追加；四条路线独立搜索后按固定顺序归并。
inline constexpr bool enable_fixed_search = true;
inline constexpr std::size_t fixed_rounds_small = 4;
inline constexpr std::size_t fixed_rounds_medium = 2;

// 开发限时模式的总时间预算，从读取输入前开始，所有工作线程共用。
// 基础种子必须先完整生成，剩余时间用于追加搜索，因此这不是强制运行时限。
inline constexpr double total_seconds = 15.0;         // 仅控制开发限时模式。
inline constexpr double quick_test_seconds = 0.50;    // 开发测试二进制使用的短预算。
inline constexpr double timed_reserve_seconds = 0.15; // 为线程汇合、输出和程序收尾预留时间。

// 追加搜索的宽度与存储上限。beam 保留多个方向，避免只沿当前赢家探索。
inline constexpr int timed_workers = 4; // 含调用线程，最多四条路线，不嵌套启动线程池。
inline constexpr std::size_t timed_beam = 6;
inline constexpr std::size_t timed_coarse_per_round = 12; // 固定模式每轮全图粗估上限。
inline constexpr std::size_t timed_refine_per_round = 3;  // 每轮共享 DDR 精评上限。
inline constexpr std::size_t timed_seen_limit = 500000;   // 每条路线的双哈希去重签名上限。

// 基础种子搜索的固定额度。graph_work = 计算节点数 + 边数 + 张量数。
// 图规模较大时缩减候选数量，不修改图依赖或合法性要求。
inline constexpr std::size_t medium_graph_work = 80000;
inline constexpr std::size_t large_graph_work = 200000;
inline constexpr int seed_variants_small = 10; // 不得超过 chain.cpp 中的 variants 数组长度。
inline constexpr int seed_variants_medium = 6;
inline constexpr int seed_variants_large = 2;
inline constexpr std::size_t seed_pool_limit = 24; // 与下方各取向的保留额度配合使用。
inline constexpr int seed_keep_time = 10;
inline constexpr int seed_keep_traffic = 6;
inline constexpr int seed_keep_other = 4;
inline constexpr std::size_t seed_legacy_keep = 4;
inline constexpr int seed_writer_repair_passes = 2;
inline constexpr int seed_legacy_rounds = 2;
inline constexpr int seed_local_rounds = 3;
inline constexpr std::size_t seed_group_limit = 24;
inline constexpr std::size_t seed_tensor_limit = 16;
inline constexpr std::size_t seed_writer_limit = 16;
inline constexpr std::size_t seed_swap_group_limit = 8;
inline constexpr std::size_t seed_legacy_move_limit = 4;
inline constexpr std::size_t seed_local_move_limit = 6;
inline constexpr std::size_t seed_reorder_bases = 3;

// 利用调度松弛量生成种子候选；这些数值是候选/重放额度，不是等待周期。
inline constexpr std::size_t slack_groups_small = 24;
inline constexpr std::size_t slack_groups_medium = 8;
inline constexpr std::size_t slack_replays_small = 8;
inline constexpr std::size_t slack_replays_medium = 4;
inline constexpr std::size_t slack_assignments = 2; // 每种分配再尝试 critical / DFS 顺序。

// 内存溢出优化的额外额度：基础搜索后最多新增 4 种排序及 2 种分配。
// 超大图跳过这部分；中图缩减窗口及热点数量，基础搜索额度保持不变。
inline constexpr std::size_t spill_ready_window = 32;   // 每步查看的 DAG 就绪节点数。
inline constexpr std::size_t spill_hotspot_tensors = 8; // 优先检查的高存活压力张量数。
inline constexpr std::size_t spill_neighborhood_nodes =
    64;                                                  // 写者闭包过大时跳过，不能拆开共享写者。
inline constexpr std::size_t spill_move_evaluations = 8; // 分配候选的粗估次数上限。
inline constexpr std::size_t spill_move_keep = 2;        // 进入共享 DDR 精评的分配候选数。
inline constexpr std::size_t spill_max_candidates = 6;   // 排序与分配候选合计上限。
inline constexpr std::size_t spill_reorder_variants =
    4; // 排序策略数量上限为 4，权重表见 spill.cpp。
// 历史开发对照遗留参数：正式构建不链接反馈模块，也不读取以下配置。
// 这些额度不参与当前提交方案的搜索路径。
inline constexpr bool feedback_enabled = true;
inline constexpr std::size_t feedback_rounds = 2;
inline constexpr std::size_t feedback_records = 8;               // 开发对照的记录数量。
inline constexpr std::size_t feedback_candidates_per_round = 12; // 开发对照的重放上限。
inline constexpr std::size_t feedback_closure_nodes = 64;        // 开发对照的前驱闭包上限。
inline constexpr std::size_t feedback_max_distance = 256;        // 开发对照的拓扑移动距离上限。
inline constexpr std::size_t feedback_refine_candidates = 4;     // 开发对照的精评上限。

static_assert(spill_ready_window > 0 && spill_reorder_variants <= 4,
              "spill ready window must be positive; at most four policies");

static_assert(timed_workers >= 1 && timed_workers <= 4, "worker helper supports 1..4");
static_assert(timed_beam > 0 && timed_coarse_per_round > 0 && timed_refine_per_round > 0,
              "search widths must be positive");
static_assert(seed_variants_small <= 10 && seed_variants_medium <= 10 && seed_variants_large <= 10,
              "seed variants cannot exceed the configured variants table");
} // namespace search_limits
} // namespace dag_chain
