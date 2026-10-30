#pragma once
#include <cstddef>

namespace dag_chain {
namespace search_limits {
// 可調搜尋參數（不是題目規則）。修改後重新編譯；正式 CLI 不增加調參旗標。
// 1000 cycles、DDR 60、L1/UB 容量在 Config，屬規則，不能以調參名義改動。

// v7 預設恢復 v5 固定候選數／輪數上限，完成後即返回。
// true 才啟用 v6 限時追加搜尋；下方 total_seconds 在 false 時不生效。
inline constexpr bool enable_timed_search = false;

// 可選目標總牆鐘預算，從讀取輸入前開始。不是每個 worker 各跑 15 秒。
// 先完成 v5 種子以保留可用方案，再利用剩餘時間搜尋。大圖種子若已超時，
// 不會再延長搜尋；此設計不是任意大小輸入都能在 15 秒內完成的硬保證。
inline constexpr double total_seconds = 15.0; // 最直接調整整體搜尋時間的位置。
inline constexpr double quick_test_seconds = 0.50; // 僅測試二進位選用，確保餘量足以進入追加搜尋。
inline constexpr double timed_reserve_seconds = 0.15; // 為 join、輸出和程序收尾留餘量。

// 追加搜尋。加大 beam 可保留更多方向，但增加記憶體與每輪成本。
inline constexpr int timed_workers = 4; // 含呼叫執行緒；不得超過 helper 的 4，也不巢狀開線程。
inline constexpr std::size_t timed_beam = 6;
inline constexpr std::size_t timed_coarse_per_round = 12; // 每輪進入全圖粗估的候選上限。
inline constexpr std::size_t timed_refine_per_round = 3; // 每輪共享 DDR 細化上限；比粗估昂貴。
inline constexpr std::size_t timed_seen_limit = 500000; // 128-bit 去重簽名上限，避免記憶體無界增長。

// v5 固定搜尋上限：全部恢復／保留 v5 原值。增大會增加固定工作量。
// graph_work = 計算 Op 數 + 邊數 + Tensor 數；以下僅縮減搜尋，不改輸入規則。
inline constexpr std::size_t medium_graph_work = 80000;
inline constexpr std::size_t large_graph_work = 200000;
inline constexpr int seed_variants_small = 10; // 不得超過 chain.cpp 的 variants 陣列長度。
inline constexpr int seed_variants_medium = 6;
inline constexpr int seed_variants_large = 2;
inline constexpr std::size_t seed_pool_limit = 24; // 需與四個取向的 keep 配額一起看。
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

// v5 slack 種子候選。只是篩選工作上限，並非可免費等待的 cycles。
inline constexpr std::size_t slack_groups_small = 24;
inline constexpr std::size_t slack_groups_medium = 8;
inline constexpr std::size_t slack_replays_small = 8;
inline constexpr std::size_t slack_replays_medium = 4;
inline constexpr std::size_t slack_assignments = 2; // 每種分配再試 critical / DFS 順序。

// v7 溢出候選的固定額外配額；不取代／壓縮上述 v5 搜尋。
// 各核心預算在完整 v5 最優方案之後，最多新增 4 種排序及 2 種分配。
// 大圖（graph_work > large_graph_work）跳過這部分；中圖縮減窗口和熱點數。
inline constexpr std::size_t spill_ready_window = 32; // 每步檢視的 DAG-ready 節點數。
inline constexpr std::size_t spill_hotspot_tensors = 8; // 存活區間壓力最大的 Tensor 數。
inline constexpr std::size_t spill_neighborhood_nodes = 64; // writer 閉包過大時跳過，不能拆開 writer。
inline constexpr std::size_t spill_move_evaluations = 8; // 分配候選粗估次數上限。
inline constexpr std::size_t spill_move_keep = 2; // 進入共享 DDR 細化的分配候選數。
inline constexpr std::size_t spill_max_candidates = 6; // 排序及分配候選合計上限。
inline constexpr std::size_t spill_reorder_variants = 4; // 可設 0..4；權重表見 spill.cpp。
// v8：完整初始搜尋之後，用官方 Step 2 相同邏輯記錄溢出並改核內順序。
// 此階段共用固定額外配額，包含多起點、少量分配及核內重排；false 關閉回饋。
// 若啟用限時追加搜尋，total_seconds 控制前段；此回饋階段另計固定工作量。
inline constexpr bool feedback_enabled = true;
inline constexpr std::size_t feedback_rounds = 2; // beam 深度，允許第一步不減少搬運。
inline constexpr std::size_t feedback_records = 8; // 每次最多採用有不同合法候選的記錄數。
inline constexpr std::size_t feedback_record_scan = 64; // 無效記錄繼續遞補，但掃描有界。
inline constexpr std::size_t feedback_trigger_region = 64; // 多樣化的核內觸發位置分組寬度。
inline constexpr std::size_t feedback_candidates_per_round = 12; // 每個狀態最多產生的候選。
inline constexpr std::size_t feedback_closure_nodes = 64; // 為合法前移補入的前驅閉包上限。
inline constexpr std::size_t feedback_max_distance = 256; // 各被移動節點在其核心跨越的步數。
inline constexpr std::size_t feedback_refine_candidates = 4; // 起點階段及各深度細化上限。
inline constexpr std::size_t feedback_beam = 3;
inline constexpr std::size_t feedback_replay_budget = 48; // 所有起點/分配/兩步搜尋共用；不含初始 trace。
inline constexpr std::size_t feedback_refine_budget = 12; // 所有階段共用的完整估時次數。
inline constexpr std::size_t feedback_replays_per_depth = 20;
inline constexpr std::size_t feedback_seed_limit = 3;
inline constexpr double feedback_seed_cycle_ratio = 1.05;
inline constexpr std::size_t feedback_mapping_candidates = 4;
inline constexpr std::size_t feedback_mapping_records = 16;
inline constexpr std::size_t feedback_mapping_nodes = 32;
static_assert(feedback_beam >= 1, "feedback beam must retain at least one state");
static_assert(feedback_seed_cycle_ratio >= 1.0, "seed cycle ratio must include the winner");

static_assert(spill_ready_window > 0 && spill_reorder_variants <= 4,
              "spill ready window must be positive; at most four policies");

static_assert(timed_workers >= 1 && timed_workers <= 4, "worker helper supports 1..4");
static_assert(timed_beam > 0 && timed_coarse_per_round > 0 && timed_refine_per_round > 0,
              "search widths must be positive");
static_assert(seed_variants_small <= 10 && seed_variants_medium <= 10 && seed_variants_large <= 10,
              "seed variants cannot exceed the configured variants table");
} // namespace search_limits
} // namespace dag_chain
