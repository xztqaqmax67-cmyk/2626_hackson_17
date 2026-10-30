# v8 搜尋上限與回饋配額

所有常用配額在 `src/search_limits.hpp`。修改既有常數後必須重新編譯，正式 CLI 沒有 `--time-limit` 等額外調參選項。

## 預設流程

`enable_timed_search=false`：先完整執行 v7 固定搜尋，再執行 v8 的溢出事件回饋。固定配額不代表固定秒數；不同圖的生成耗時不同。

`feedback_enabled=true`：預設開啟新回饋；設為 false 可關閉回饋並直接輸出前段搜尋結果。保留其他預設值時，這就是 v7 的搜尋流程。

## v8 回饋配額

| 參數 | 預設 | 調整影響 |
|---|---:|---|
| feedback_rounds | 2 | 最多接受重排的輪數；每輪重新取得溢出事件 |
| feedback_records | 8 | 每輪按搬運成本篩選的事件數 |
| feedback_candidates_per_round | 12 | 每輪完整 Step 2 重播候選數 |
| feedback_refine_candidates | 4 | 每輪 cycles 細化候選數 |
| feedback_closure_nodes | 64 | 為合法前移／後移補入的前驅或後繼閉包上限 |
| feedback_max_distance | 256 | 全域拓撲序中的最大移動距離，不是核內序號差 |

例如可將 records 從 8 改為 16、candidates_per_round 從 12 改為 24、refine_candidates 從 4 改為 8，探索更多事件；這只是修改方法，未套用也未驗證效果。只提高候選總配額，不一定能找到更多合法變換。增加任一配額都可能增加生成時間。

目前高成本事件在合法性篩選前選出，可能包含同一 Tensor 的重複事件；距離限制也可能排除長生命期的調整。這些是搜尋覆蓋範圍限制，不是官方規則。不得為改善分數而修改容量、軟閾值、DDR 帶寬或同步延遲。

## 前段 v7 搜尋配額

`seed_*`、`slack_*`、`spill_*` 保留 v7 原值。最常調整的是：

- `seed_local_rounds=3`、`seed_local_move_limit=6`：局部搜尋回合與每回合移動數。
- `seed_reorder_bases=3`：重新排序的起始方案數。
- `spill_move_evaluations=8`、`spill_move_keep=2`、`spill_max_candidates=6`：v7 通用壓力候選的預篩、移核保留與總數。若將 keep 增到 4，可同步把總數增到 8，容納原 4 種排序。
- `seed_variants_small/medium/large=10/6/2`，變體表目前最多 10 種；`spill_reorder_variants=4` 最多 4 種，不能僅增大數字來創造不存在的策略。
- `seed_pool_limit=24` 是剪枝觸發數，需連同保留配額 `seed_keep_time=10`、`seed_keep_traffic=6`、`seed_keep_other=4` 一起看。四個取向的聯集配額為 10+6+2×4=24，原方案補回後仍可能更多。

`medium_graph_work=80000`、`large_graph_work=200000` 控制前段的部分搜尋量；新回饋仍按上述固定事件配額進行。其他策略表與 regions 內部界限保留在對應程式中。

## 可選限時模式

```cpp
inline constexpr bool enable_timed_search = true;
inline constexpr double total_seconds = 30.0;
```

只改秒數而不開啟開關不會生效。限時預算從讀圖前開始，先完成前段固定搜尋，再使用剩餘時間追加；各 worker 共用截止點。`timed_workers` 僅控制追加階段，範圍 1～4；beam、coarse、refine、seen 配額是每個 worker 各自的配額。

**v8 的事件回饋在這之後執行，另計固定工作量。** 設 30 秒不代表整個程式硬限制為 30 秒；前段固定搜尋本身也不被截止中斷。`npu_sched_quick` 的前段限時預算是 quick_test_seconds，不能用它代表正式版。

## 建置、診斷與評分

從 v8 目錄執行：

```powershell
cmake -S . -B build_msvc -G "Visual Studio 17 2022" -A x64 -DCHAIN_BUILD_TESTS=ON
cmake --build build_msvc --config Release --parallel 4
ctest --test-dir build_msvc -C Release --output-on-failure
./build_msvc/bin/npu_sched.exe input.json -n 5 -o plan.json
./build_msvc/bin/spill_diagnostics.exe input.json plan.json spill_trace.json
```

`--parallel 4` 是編譯工作數，`-n 5` 是模擬 NPU 核數上限，均不是搜尋執行緒參數。源碼壓縮包沒有 tests，應使用預設 `CHAIN_BUILD_TESTS=OFF` 建置；開發測試需完整目錄。

`run_v8_official.py` 只接受固定模式。調參實驗請用新目錄，例如 `--out runs/tuning_trial`，避免與既有 SHA256 記錄混用。測試工具的 90 秒生成 watchdog 是測試超時，並非搜尋上限。限時模式需要單獨生成方案並呼叫官方評分器，或另行準備能正確記錄預算的 harness。

`check_official_spill.py` 核對 C++ 與官方 Step 2 的完整記錄；預設使用本地 v7 歷史方案和合成圖。`check_final_spill.py` 核對預設目錄中的 v8 最終方案，對與先前完全相同且雜湊一致的方案沿用已完成證據。`summarize_v8.py` 讀取預設 `runs/official_all/`，需要本地 v7 原始結果，並寫回標準報告；新參數實驗應在獨立副本整理，保留本次預設設定的結果。
