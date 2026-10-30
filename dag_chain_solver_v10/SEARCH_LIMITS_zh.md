# v10 搜尋上限與分組參數（0.10.1）

整體搜尋配額位於 `src/search_limits.hpp`；時間線候選的閉包／距離細項位於 `src/timing_feedback.hpp` 的 `TimingFeedbackLimits`。修改後重新編譯。這些是搜尋參數，不是官方規則。正式 CLI 沒有額外調參旗標。


## v10 新階段的額外預算

以下均在完整 v9 流程之後，預設不是硬性秒數截止。

| 參數 | 預設 | 作用 |
|---|---:|---|
| v10_tensor_limit | 12 | 參與共用關係候選的 Tensor 數 |
| v10_group_evaluations | 48 | 分組移動的便宜評估總上限 |
| v10_group_keep_per_direction | 2 | 每個分組方向保留候選數 |
| v10_group_move_nodes | 64 | 移動群組節點上限；writer 閉包不可截斷 |
| v10_split_atoms | 8 | 熱點拆分可考察的 writer atom 數 |
| v10_order_candidates | 4 | 由溢出熱點增加的純排序候選上限 |
| v10_refine_candidates | 8 | 新候選進入完整舊細估的總上限 |
| v10_feedback_starts | 3 | 不同取向的回饋起點上限 |
| v10_feedback_replays | 48 | 所有起點共用的重播額度 |
| v10_feedback_refinements | 12 | 所有起點共用的細估額度 |
| v10_exact_candidates | 3 | 0.10.0 原精算前綴，包含本次重新求解的 v9 入口 |
| v10_exact_pool_candidates | 6 | 擴充精算候選池總數，已包含原 3 個候選 |
| v10_timing_order_candidates | 4 | 固定起點的時間線調序候選上限 |
| v10_exact_spill_candidates | 4 | 同一固定起點的溢出調序候選上限 |

預設最多 `6 + 4 + 4 = 14` 次完整多核方案精算；原 3 個候選已包含在 6 個中，不再額外相加。所有名額按正式核心序列去重，實際呼叫可能更少。這是一次生成程序的完整方案數，不是每核各 14 次。前段的 Step2 重播、近似細估仍有各自配額，不能把 14 解讀成所有估價的總呼叫數。

擴充池完整保留原 3 個候選及其順序，先比較精確 cycles，cycles 相同才比較總搬運。尾段的時間線／溢出候選均從擴充池勝出方案產生，固定起點、單輪、交替評估；接受新方案後不重新展開下一輪。較多搬運不能單獨否決較低 cycles 的合法方案，同 cycles 時只接受較少總搬運。原 3 個候選及其保底結果都由本次執行重算，不載入歷史 plan。

`v10_exact_candidates` 必須至少為 1；要保留與 0.10.0 相同的前綴，請維持預設 3。`v10_exact_pool_candidates` 不得小於它。調高任一精算／尾段配額會增加完整 Step1／2／3 與全核計時次數，沒有保證換得更佳分數。

多起點回饋每階段最多分配 2 次細估，保留後續排序修復的機會；未用完額度可供後面的起點使用。尾段若勝出，另做 1 次近似細估更新輸出診斷；該估計不否決已精算確認的改進，`stats.refined` 因此最多為前段 8 次加這 1 次。

## 時間線調序細項

以下預設位於 `src/timing_feedback.hpp`，不是 `src/search_limits.hpp`。它們限制候選生成工作量，最後仍由完整精確計時決定是否接受。

| TimingFeedbackLimits 欄位 | 預設 | 作用 |
|---|---:|---|
| previous_pipe_ops | 4 | 每個原始運算在同 Pipe 最多回看幾個較早運算，尋找提前插入位置 |
| closure_attempts | 32 | 最多嘗試建立幾個前驅閉包 |
| closure_nodes | 64 | 單次移動的完整前驅閉包節點上限 |
| closure_edges | 2048 | 建立單次閉包時最多檢查的前驅邊數 |
| per_core_distance | 256 | 每核心允許跨越的核內順序距離 |

閉包包含所有核心上的必要前驅，保持原分核及同核群組資料；群組不會被當作必須連續執行的原子區塊。超過任一上限就跳過該候選，不截斷依賴，也不刪除原 DAG 邊。原始前驅完成時間僅用作就緒時間下界；COPY、1000 cycles 跨核等待、容量重用依賴都留給完整重播處理。

## 計時與比較

`v10_stats.exact_seconds` 包括擴充精算池及整個尾段；`tail_seconds` 是其中的尾段子區間，不能將兩者相加。這些是本機實際經過的秒數，不是模擬硬體 cycles。預設固定工作量，**沒有每點 15 秒硬限制**。

0.10.1 預設配額已完成兩個階段各 40 組、共 80 組官方比較。exact_pool 平均單核加速比為 **1.947173×**（相對 0.10.0 的 1.946856×，+0.0163%）；完整 exact 為 **1.949478×（+0.1347%）**，24 組 cycles 改善、16 組相同、無退步，總 COPY +0.0280%、溢出 COPY +0.0474%。完整生成平均 **9.982 秒**、範圍 **0.126～29.273 秒**；0.10.0 歷史平均為 **9.411 秒**。擴充池版本本輪平均為 8.362 秒；因各版本獨立量測及環境波動，不能據此宣稱增加精算使生成變快。

完整版本平均 `exact_seconds=1.484`，其中 `tail_seconds=0.826` 已包含在內。40 組共 493 次精算；尾段產生 272 個候選、去重 11 個、實際精算 261 次。`tail_accepted=40` 是搜尋途中更新最佳解的次數，不是改善測試點數；尾段最終相對 pool 改善 24 點。提高名額未必按比例改善分數，調參應同時比較 cycles、搬運及本機生成時間。

新測試使用 `scripts/run_v10_upgrade.py`，分別保存於 `runs/upgrade/official_exact_pool`、`runs/upgrade/official_exact`；彙整使用 `scripts/summarize_v10_upgrade.py`，寫至 `reports/upgrade/RESULTS_V10_1.json` 及同名中文表。新增 144／144 組 C++／官方差分一致、0 組失敗；其中 16 組是雙方一致拒絕，不算成功生成的合法方案，詳見 [0.10.1 完整驗證紀錄](reports/upgrade/VALIDATION_V10_1.json)。原 `reports/RESULTS_V10*.json/md`、`runs/official_exact` 與舊 dist 是 0.10.0 歷史證據，保留不覆寫；新版源碼包為 `dist/dag_chain_solver_v10_1.zip`。

搜尋狀態去重同時考慮核心序列及同核群組約束，避免丟失不同的後續移動自由度；最後精確計時按正式核心序列去重，避免同一個輸出方案佔用多個精算名額。

前兩階段用近似 cycles／總搬運保守篩選，最後精確階段用官方 cycles 優先；不增加「總搬運不得變多」的合法性限制。任何搜尋上限只是限制可嘗試的候選，不允許修改官方硬體規則。0.10.1 的實體版本查找二分優化不屬於搜尋上限：每條使用邊由 O(S) 改為 O(log S)，其中 S 是同一 Tensor 的溢出次數，保留完全相同的版本選擇規則。

## 保留的 v9 搬運分組階段

| 參數 | 預設 | 作用 |
|---|---:|---|
| communication_enabled | true | 啟用新增階段；false 保留完整v8求解流程 |
| communication_group_nodes | 64 | 切段時每組的節點數上限；必要writer合併可能超過 |
| communication_cut_window | 64 | 切邊動態規劃每步回看的節點數 |
| communication_variant_limit | 2 | 最多2種：scale0.5＋footprint限制；scale1.0不以footprint聯集硬限 |
| communication_refine_candidates | 6 | 全部核心預算共用的候選細化上限 |
| communication_post_feedback | true | 最多對一個新分組候選追加有界溢出回饋 |

可用核心上限N時，新分組候選至多 `(N-1) × variant_limit × 2`，兩個順序為critical／DFS，因此5核預設至多16個。群組大小、Pipe工作量與保守候選的Tensor footprint限制是控制搜尋取向的啟發式；必要單節點或writer群組不能因啟發式上限而使原合法輸入被拒絕。

Tensor聯集不等於同時駐留峰值，第二種候選不將聯集大小作為硬門檻；兩種候選仍都經過完整溢出／時間檢查。入選的中間候選可暫時增加搬運，最終結果仍受原入口的搬運和估時上界限制。

提高group_nodes／cut_window可能保留更多昂貴依賴，但增加DP成本、群組負載與駐留壓力。提高refine_candidates會讓更多候選進入昂貴估時。只增加總配額不保證分數改善。這一階段以搬運權重連鏈，沒有將最晚完成時間重新加入主排序。

## 前段v8與後段回饋

完整v8保底仍有48次候選Step2重播／12次細化的總額度；入口trace另計。若開啟communication_post_feedback，新分組階段最多另外呼叫一次相同有界回饋，因此不能把48／12解讀為整個v9的總工作量。新增分組階段另有入口trace、最多6個入選候選trace及後處理的最終核對trace；候選細化上限6，與回饋內12次分開記錄。

回饋預設beam寬3、深2，每深度最多20次重播、每階段4次細化；每狀態最多掃描64筆多樣化事件，保留8筆有用記錄並生成12候選。`feedback_max_distance=256` 是核內跨越步數，完整DAG閉包上限64。移核上限32個節點，遇到更大的強制同核群組會跳過，不會拆開它。

前段 `seed_*`、`slack_*`、`spill_*` 配額沿用v8。群組約束只作用於新增分組方案；保底v8的歷史搜尋規則保持原樣。

## 可選限時模式

預設 `enable_timed_search=false`，固定工作量完成即返回。設true並修改total_seconds才會啟用前段追加搜尋；它不包含後續v8回饋、v9分組及v10階段，因此不是整個程式的硬截止。`-n`是NPU核心上限，`timed_workers`是前段追加搜尋執行緒，建置`--parallel`是編譯工作數，三者不同。

所有正式報告使用預設固定模式，生成時間不含官方評分。測試工具90秒watchdog是測試超時，不是搜尋上限。使用新目錄保存調參實驗，原始比較結果不要覆寫。
