# 如何調整 v7 的搜尋上限

常用搜尋上限集中於 [src/search_limits.hpp](src/search_limits.hpp)。**修改原有常數的值，再重新編譯正式執行檔。** 不要另外新增同名變數。正式 CLI 沒有 `--time-limit`、`--beam` 等調參選項；`-n` 表示 NPU 可用核心數，不是搜尋執行緒數。

目前 GitHub／原始碼包中的預設仍是已完成 40 組評測的設定：`enable_timed_search=false`，保留 v5 原有固定搜尋上限，再加少量 v7 溢出候選。下文的加大範例只是修改方式，尚未實測，沒有套用到預設值。

## 1. 最常見的修改：啟用約 15／30 秒搜尋

在頭檔中找到這兩行，將值改為：

```cpp
inline constexpr bool enable_timed_search = true;
inline constexpr double total_seconds = 30.0; // 想設約 15 秒就改成 15.0。
```

只改 `total_seconds` 而保留 `enable_timed_search=false`，**不會增加搜尋時間**。想恢復本次固定工作量版，把開關改回 `false` 並重新編譯即可。

限時模式的流程是「讀圖 → 完整 v7 固定搜尋 → 有餘量才追加搜尋 → 寫出方案」。完整固定搜尋包含 v5 種子、regions、slack 及 v7 spill 候選。所有追加 worker 共用從讀圖前開始的牆鐘預算，並非每個 worker 各跑 30 秒。預留 `timed_reserve_seconds=0.15` 秒用於收尾；固定搜尋本身不會被中斷，所以大圖可能超過目標秒數。候選耗盡時可提前結束，不會等待補滿秒數。

正式測試請使用 `npu_sched`。`npu_sched_quick` 只在開啟限時模式時改用 `quick_test_seconds=0.50`，不會使用 `total_seconds`；預設固定模式下兩者執行相同搜尋。`chain_diagnostics` 始終執行固定搜尋，不能用它驗證 15／30 秒模式。

## 2. 維持固定工作量，但增加候選數

保留 `enable_timed_search=false`，可以單獨調整某一段搜尋。例如增加局部搜尋：

```cpp
inline constexpr int seed_local_rounds = 5;             // 原本 3：增加回合。
inline constexpr std::size_t seed_local_move_limit = 10; // 原本 6：每回合多試移動。
inline constexpr std::size_t seed_reorder_bases = 4;     // 原本 3：多選一個起點重排。
```

這些參數控制固定工作量，不直接指定秒數；不能預期「回合翻倍」恰好讓實際耗時翻倍。建議每次只改一組參數，記錄生成秒數、官方 cycles、總搬運及溢出搬運。搜尋更多也不保證官方結果更好，因為內部選優使用估價模型。

## 3. 只增加 v7 的溢出優化搜尋

以下範例保留 4 種排序策略，增加移核候選的預篩與最終保留數：

```cpp
inline constexpr std::size_t spill_move_evaluations = 16; // 原本 8：移核候選粗估數。
inline constexpr std::size_t spill_move_keep = 4;         // 原本 2：最多保留 4 種移核方案。
inline constexpr std::size_t spill_max_candidates = 8;    // 原本 6：容納最多 4 種排序 + 4 種移核。
```

這三個值需要一起看：只提高 `spill_move_keep`，候選仍可能被 `spill_max_candidates` 擋住；只提高總上限，也不會自動產生更多策略。`spill_reorder_variants` 目前只有 4 種策略，不能改成 5，除非同步擴充 `spill.cpp` 的權重表和檢查。

若希望擴大候選來源，可以增加 `spill_hotspot_tensors`（壓力 Tensor 數）、`spill_neighborhood_nodes`（可搬移群組大小），或 `spill_ready_window`（每步比較的就緒節點數）。這會增加候選生成成本，並非全部候選都會送去細化。

想停用本版新溢出策略，設 `spill_max_candidates=0`；其他 v5 固定搜尋仍保留。每個核心預算分別受配額限制，新候選還可能經過既有 writer 排序修復，故「最多 6 個原始候選」不等於「最多 6 次細化」。

## 4. 參數總表與相互限制

### 原有固定搜尋（預設值）

| 參數 | 值 | 用途與調整注意 |
|---|---:|---|
| medium_graph_work / large_graph_work | 80000 / 200000 | 以計算 Op + 邊 + Tensor 數分級；保持 medium ≤ large |
| seed_variants_small / medium / large | 10 / 6 / 2 | 分解變體數；保留正整數且最多 10，表中沒有第 11 種 |
| seed_pool_limit | 24 | 觸發候選池剪枝的數量，不是最終候選硬上限 |
| seed_keep_time / traffic / other | 10 / 6 / 4 | 四個取向的保留數；other 用於兩個取向，保留正整數 |
| seed_legacy_keep | 4 | 原搜尋方案保留數 |
| seed_writer_repair_passes | 2 | writer 排序修復遍數 |
| seed_legacy_rounds / seed_local_rounds | 2 / 3 | 兩段局部搜尋的回合數 |
| seed_group_limit / tensor_limit / writer_limit | 24 / 16 / 16 | 局部群組、Tensor、writer 篩選數；名稱均以 seed_ 起頭 |
| seed_swap_group_limit | 8 | 參與兩兩交換的群組數，增大會增加配對數 |
| seed_legacy_move_limit / seed_local_move_limit | 4 / 6 | 每回合深入評估的移動數 |
| seed_reorder_bases | 3 | 重排起始方案數 |
| slack_groups_small / slack_groups_medium | 24 / 8 | slack 群組預篩數 |
| slack_replays_small / slack_replays_medium | 8 / 4 | slack 完整重播數 |
| slack_assignments | 2 | 保留分配數，每種再試兩種排序 |

候選池四個取向保留聯集最多為 `seed_keep_time + seed_keep_traffic + 2 * seed_keep_other`（預設 24）；另有原搜尋方案補回流程。若要擴大池子，連同保留配額一起調，不要只把 `seed_pool_limit` 當成 beam 大小。例如保留配額 14／8／5，對應聯集上限 32，可搭配 `seed_pool_limit=32`。保留方案可能重複，實際數量可以更少。

圖規模門檻只縮減搜尋成本，不改合法輸入範圍。chain.cpp 的局部搜尋在超過 large 門檻時會跳過；超過 medium 門檻時 spill 的 ready 窗口與熱點數減半（整數除法，ready 窗口至少 1），超過 large 時整段 spill 跳過。小於等於門檻的圖仍使用較小圖的分支。

### v7 溢出候選（預設值）

| 參數 | 值 | 用途與限制 |
|---|---:|---|
| spill_ready_window | 32 | 每步比較的 DAG-ready 節點數，必須 ≥ 1 |
| spill_reorder_variants | 4 | 排序策略數，可設 0～4；設 0 仍可嘗試移核 |
| spill_hotspot_tensors | 8 | 按存活區間壓力選取的 Tensor 數 |
| spill_neighborhood_nodes | 64 | 可搬移 writer 閉包的節點上限；超過就跳過，不能拆開 writer |
| spill_move_evaluations | 8 | 移核候選重新排序並粗估的數量 |
| spill_move_keep | 2 | 最多保留的移核候選數，應不大於 move_evaluations |
| spill_max_candidates | 6 | 排序與移核候選總上限；0 關閉整段新策略 |

### 可選限時延伸（僅 enable_timed_search=true 生效）

| 參數 | 值 | 用途與限制 |
|---|---:|---|
| total_seconds | 15.0 | 正式執行檔的目標總牆鐘預算，設有限非負值 |
| timed_reserve_seconds | 0.15 | 收尾餘量，保留非負值；通常小於 total_seconds |
| timed_workers | 4 | 追加搜尋工作數（含呼叫執行緒），只能 1～4 |
| timed_beam | 6 | 每個 worker 保留的搜尋起點數，必須 > 0 |
| timed_coarse_per_round | 12 | 每個 worker 每輪粗估候選上限，必須 > 0 |
| timed_refine_per_round | 3 | 每個 worker 每輪細化上限，必須 > 0，通常不大於 coarse |
| timed_seen_limit | 500000 | 每個 worker 的去重簽名上限；達到後該 worker 停止搜尋 |
| quick_test_seconds | 0.50 | 僅 quick 測試執行檔的限時預算 |

`timed_workers` 只控制追加階段，不控制先前「2～指定核數」的固定搜尋並行工作；後者最多 4 個，使用 `parallel_jobs.hpp`。兩階段依序執行，不巢狀開搜尋執行緒。增大 beam／去重上限會提高記憶體用量；增加單輪候選也可能讓同一時間預算完成更少輪。

演算法還包含策略表、排序權重與 regions 模組中的內部枚舉界限；它們不等同於這份頭檔的集中配額。若要擴充候選類型或 regions 演算法，需要閱讀對應實作，不能只修改數字便假設新增策略已存在。

## 5. 修改後重新編譯

以下指令從 v7 資料夾執行。已經有 build_msvc 時，只需要 build 指令；首次建置先 configure：

```powershell
cmake -S . -B build_msvc -G "Visual Studio 17 2022" -A x64 -DCHAIN_BUILD_TESTS=ON
cmake --build build_msvc --config Release --parallel 4
ctest --test-dir build_msvc -C Release --output-on-failure
./build_msvc/bin/npu_sched.exe ./references/hackathon_20261030/data/case_00.json -n 5 -o ./runs/tuning_trial/case_00_n5.json
```

`--parallel 4` 只控制編譯並行數，不是求解器的搜尋執行緒數。`cmake` 需在 PATH 中；可以從 Visual Studio 的開發人員終端機執行。執行上例需完整開發目錄及題目附件。一般 CMake 環境可使用 `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 和 `cmake --build build --config Release --parallel 4`。

只有原始碼壓縮包時，使用預設 `CHAIN_BUILD_TESTS=OFF` 建置即可；該包沒有 tests、scripts、reports 或題目資料。想跑開發測試，應使用 GitHub 上的完整開發目錄。

**修改頭檔後不重新編譯，現有 exe 仍使用舊設定。** 確認執行路徑是剛建置的 `npu_sched`，不要誤用舊版本目錄或 quick 版。

## 6. 調參後評分與保留結果

固定模式可以用既有腳本評分全部 40 組，為每次設定指定新的輸出目錄：

```powershell
python scripts/run_v7_official.py --solver build_msvc/bin/npu_sched.exe --out runs/tuning_fixed_trial
```

腳本記錄來源和執行檔 SHA256。更換參數後沿用已有結果目錄會被拒絕，應另取新名稱，不要把舊結果冒充新設定的結果。其 90 秒生成 timeout 是測試程式的看門狗，不是求解器的搜尋上限；若研究更長預算，需要另外檢查測試工具的 timeout。

目前 `run_v7_official.py` **只接受固定模式**，會拒絕 `enable_timed_search=true`。限時方案可先用正式執行檔生成，再逐個呼叫原版評分器，例如：

```powershell
python references/hackathon_20261030/code/multicore_cut_evaluate_problem_2.py references/hackathon_20261030/data/case_00.json runs/tuning_trial/case_00_n5.json --config references/hackathon_20261030/data/config.txt -o runs/tuning_trial/result.json --trace-output runs/tuning_trial/trace.json --log-output runs/tuning_trial/log.txt
```

這只是單點命令示例；新的限時批次實驗應另寫或調整 harness，正確記錄模式、實際預算、生成時間及 timeout，不能用舊的固定模式報表標題。固定模式的 CLI／合成圖測試包含「重跑結果相同」斷言，開啟限時搜尋後不適用於要求 byte identity 的測試；時間截止會使完成候選數不同。

目前 40 組報告對應原本預設設定。調參後必須重新生成、重新官方評分，才能宣稱新分數。`summarize_v7.py` 也會寫回既有報告，且需要本地 v5、v6 歷史原始結果作比較；比較實驗請在獨立副本整理，保留預設版報告。

容量、DDR 帶寬、跨核同步延遲和官方換出策略屬於題目規則，不是搜尋參數，不能藉調參修改。
