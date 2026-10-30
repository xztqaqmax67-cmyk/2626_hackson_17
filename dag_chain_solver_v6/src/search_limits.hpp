#pragma once
#include <cstddef>

namespace dag_chain {
namespace search_limits {
// 可調搜尋參數（不是題目規則）。修改後重新編譯；正式 CLI 不增加調參旗標。
// 1000 cycles、DDR 60、L1/UB 容量在 Config，屬規則，不能以調參名義改動。

// 目標總牆鐘預算，從讀取輸入前開始。不是每個 worker 各跑 30 秒。
// 先完成 v5 種子以保留可用方案，再利用剩餘時間搜尋。大圖種子若已超時，
// 不會再延長搜尋；此設計不是任意大小輸入都能在 30 秒內完成的硬保證。
inline constexpr double total_seconds = 30.0; // 最直接調整整體搜尋時間的位置。
inline constexpr double quick_test_seconds = 0.50; // 僅測試二進位選用，確保餘量足以進入追加搜尋。
inline constexpr double timed_reserve_seconds = 0.15; // 為 join、輸出和程序收尾留餘量。

// 追加搜尋。加大 beam 可保留更多方向，但增加記憶體與每輪成本。
inline constexpr int timed_workers = 4; // 含呼叫執行緒；不得超過 helper 的 4，也不巢狀開線程。
inline constexpr std::size_t timed_beam = 6;
inline constexpr std::size_t timed_coarse_per_round = 12; // 每輪進入全圖粗估的候選上限。
inline constexpr std::size_t timed_refine_per_round = 3; // 每輪共享 DDR 細化上限；比粗估昂貴。
inline constexpr std::size_t timed_seen_limit = 500000; // 128-bit 去重簽名上限，避免記憶體無界增長。

// v5 種子搜尋：保持原值，以保留原搜尋起點。增大會吃掉追加搜尋的時間。
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

static_assert(timed_workers >= 1 && timed_workers <= 4, "worker helper supports 1..4");
static_assert(timed_beam > 0 && timed_coarse_per_round > 0 && timed_refine_per_round > 0,
              "search widths must be positive");
static_assert(seed_variants_small <= 10 && seed_variants_medium <= 10 && seed_variants_large <= 10,
              "seed variants cannot exceed the configured variants table");
} // namespace search_limits
} // namespace dag_chain
