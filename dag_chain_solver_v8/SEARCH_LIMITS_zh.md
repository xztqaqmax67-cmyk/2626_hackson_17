# v8 0.8.1 搜尋上限

修改 `src/search_limits.hpp` 後重新編譯。正式 CLI 不增加調參旗標。以下都是搜尋參數，不是題目規則；禁止調整硬體容量、軟閾值、DDR 帶寬或同步延遲來改善分數。

## 預設流程

`enable_timed_search=false`：完整 v7 固定搜尋 → v8 多起點／移核／有界重排回饋。`feedback_enabled=false` 可關閉最後回饋，前段勝者不變。固定工作量不保證任意圖在固定秒數內完成。

## 回饋上限

| 參數 | 預設 | 意義 |
|---|---:|---|
| feedback_replay_budget | 48 | 所有起點、移核、深度合計的候選 Step 2 重播上限；不含入口 trace |
| feedback_refine_budget | 12 | 所有階段合計的完整估時上限 |
| feedback_rounds | 2 | 重排搜尋深度；允許中性第一步 |
| feedback_beam | 3 | 保留的搜尋狀態數，至少 1 |
| feedback_replays_per_depth | 20 | 每個深度另設的重播上限 |
| feedback_refine_candidates | 4 | 起點階段及每個深度的估時上限 |
| feedback_candidates_per_round | 12 | 每個狀態的重排候選上限，不是全部搜尋總量 |
| feedback_record_scan | 64 | 每次候選生成最多掃描的多樣化事件數 |
| feedback_records | 8 | 每次最多使用能產生新合法候選的事件數 |
| feedback_trigger_region | 64 | 觸發位置多樣化的核內區域寬度 |
| feedback_closure_nodes | 64 | 合法前移／後移需要移動的完整閉包上限 |
| feedback_max_distance | 256 | 每個移動節點在其核心跨越的步數；不計其他核心無關操作 |
| feedback_seed_limit | 3 | 前段保留的近優起點數（含可能與入口重複的最優方案） |
| feedback_seed_cycle_ratio | 1.05 | 額外起點的估計 cycles 至多是最優的 1.05 倍，至少 1 |
| feedback_mapping_candidates | 4 | 原始溢出熱點的移核候選上限 |
| feedback_mapping_records | 16 | 移核候選採用的不同核心／Tensor 熱點上限 |
| feedback_mapping_nodes | 32 | 單個移核候選的完整 writer 群組節點上限 |

要增加完整回饋工作量，先調 `feedback_replay_budget`、`feedback_refine_budget`，再同步考慮每深度配額與 beam／深度；只增加總額度而其餘上限較小，可能不會多做工作。要提高覆蓋而不增加重播數，可先調掃描事件、區域分散或距離。這些調整不保證分數提升。

例如把總重播從 48 增至 72、每深度從 20 增至 30，會容納更多候選；把總細化從 12 增至 18、每階段從 4 增至 6，會多評估時間。這僅是調整方法，並非目前預設或已驗證的最佳值。

候選多樣化和完整依賴閉包也有 CPU 成本。新增 stderr `feedback_stats` 可確認實際耗用與淘汰原因，避免只憑設定值推測執行時間。中性狀態不代表可直接輸出；最終仍以入口方案的總搬運與估計 cycles 為上界。

## 前段搜尋與可選限時模式

`seed_*`、`slack_*`、`spill_*` 保留原搜尋上限。常調參數包括 `seed_local_rounds=3`、`seed_local_move_limit=6`、`seed_reorder_bases=3` 和 v7 的 `spill_move_evaluations=8`／`spill_move_keep=2`／`spill_max_candidates=6`。變體數不得超過程式中的策略表長度。

```cpp
inline constexpr bool enable_timed_search = true;
inline constexpr double total_seconds = 30.0;
```

只改秒數但不開開關不生效。限時預算從讀圖前開始，先保留完整固定搜尋結果，再利用剩餘時間追加；worker 共用截止點。`timed_workers` 1～4 控制搜尋執行緒；`-n` 控制模擬 NPU 核數；`--parallel` 是編譯工作數。

回饋另計固定工作量，所以 30 秒不是整個程式的硬截止。可選限時模式目前沒有收集額外近優起點，但仍有移核及兩步重排。`npu_sched_quick` 是開發測試工具，不代表正式版時間。

## 編譯與測試

```powershell
cmake -S . -B build_msvc -G "Visual Studio 17 2022" -A x64 -DCHAIN_BUILD_TESTS=ON
cmake --build build_msvc --config Release --parallel 4
ctest --test-dir build_msvc -C Release --output-on-failure
./build_msvc/bin/npu_sched.exe input.json -n 5 -o plan.json
python scripts/run_v8_official.py --out runs/official_upgrade
```

源碼 ZIP 未含 tests，使用預設 `CHAIN_BUILD_TESTS=OFF`。正式40組工具只接受預設固定模式，90秒 watchdog 是測試超時，不是演算法截止。調參用新的 `--out` 目錄，避免與已保存來源 hash 混用；標準新版摘要工具讀取 `runs/official_upgrade/`，其他實驗需要另行整理。
