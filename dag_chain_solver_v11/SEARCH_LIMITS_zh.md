# v11 搜尋上限與整體時間預算（0.11.1）

修改源碼參數後重新編譯；正式 CLI 不增加搜尋調參旗標。這些上限只限制搜尋，不可修改官方的 1000 cycles 同步、60 bytes/cycle DDR 或 L1／UB 容量。

## 如何調整整個程序時間

修改 `src/search_limits.hpp` 的下列參數。所有秒數都以**讀取輸入前的同一時鐘起點**計算，包含讀檔、解析及前面階段；不是各段各用一份時間。

| 參數 | 預設 | 意義 |
|---|---:|---|
| generation_budget_enabled | true | 啟用整體合作式時間預算 |
| generation_target_seconds | 20.0 | 整個程序的目標時間，含收尾餘量 |
| generation_seed_stop | 8.0 | 初始候選搜尋截止點 |
| generation_feedback_stop | 10.0 | 溢出回饋截止點 |
| generation_communication_stop | 11.5 | 搬運群組搜尋截止點 |
| generation_v10_stop | 14.5 | v10 候選／精算截止點 |
| generation_search_stop | 18.0 | 貪心／SA／精算停止開啟新工作的截止點 |

若某階段提前完成，立即進入下一段並使用剩餘時間。較小輸入可遠早於 15 秒返回；不會 sleep 補足時間。18～20 秒預留約 2 秒給在途完整評分、最後合法性核對及輸出。

**這不是任意圖都能在 20 秒返回的硬保證。** 首個可用方案、輸入處理、完整官方規則精算及輸出具有不可任意截斷的區間；極大圖或系統負載可能超出目標。只在安全邊界停止，不能為趕時間輸出未完成或非法方案。實際生成時間以外部程序起訖量測，官方外部評分時間另外記錄。

調整目標時應一起修改各截止點並保留有序關係。單改 `generation_target_seconds` 不會自動縮放其他階段，也不會增加搜尋時間。源碼的 `static_assert` 檢查截止點順序。正式預設的 `generation_*` 與舊 `total_seconds=15` 不同：後者只控制舊版前段追加搜尋。

若要做固定工作量比較，設定 `generation_budget_enabled=false`，同時保持 `enable_timed_search=false`。這時各函式不傳 deadline，仍使用原固定搜尋上限與完整流程；不保證 20 秒內完成。不要只修改舊 `total_seconds` 以為已限制全程。

## SA 的剩餘時間分配

在 `src/annealing_search.cpp`，貪心候選處理後，將到整體搜尋截止點的剩餘時間分成：55% 用於退火候選生成、原 DAG 排程及便宜估價；其餘留給候選的完整精算。這兩段仍共用原始起點。

每次底層退火另外預留 `min(0.25 秒, 當時候選預算剩餘時間的 20%)`，讓該 run 停止後還有機會匯出並排出最佳分核。這是留時間的啟發式，不能保證每個候選一定來得及排程。固定種子不依賴時間，但截止位置依本機速度和負載而變，重複執行可能產生不同答案。

## SA 的工作量上限

`src/search_limits.hpp` 控制總個數；時間不足時可以更早停止。

| 參數 | 預設 | 作用 |
|---|---:|---|
| annealing_variants | 2 | scale 0.5 加 Tensor 聯集啟發式；scale 1.0 不加聯集硬限制，允許 0～2 |
| annealing_restarts | 2 | 每個變體從其貪心分核重新開始的次數上限 |
| annealing_order_variants | 2 | critical／DFS 原 DAG 排程，允許 0～2 |
| annealing_exact_candidates | 4 | 全部 SA run 共用的精算候選上限 |

`src/annealing.hpp` 的 `AnnealingLimits` 控制每個 run：

| 欄位 | 預設 | 作用 |
|---|---:|---|
| proposals | 1024 | 提案步數；接受、拒絕與跳過均消耗一步 |
| move_nodes | 128 | 單次完整 move／swap 的合計節點上限 |
| node_touches | 65536 | 前向移動節點累計上限，不包括撤銷 |
| retained | 4 | 匯出的不同分核上限 |
| swap_percent | 25 | swap 提案機率百分比，其餘為 move |
| route_weight | 0.03 | 額外的邊界搬運／60 權重 |
| balance_weight | 0.01 | 各 Pipe 歸一平方負載和權重 |
| start_temperature | 0.02 | 起始溫度，乘 `max(1,初始能量)` |
| end_temperature | 0.0001 | 跑滿步數的終點溫度，同上縮放 |
| seed | 0x3d25c19b7a6e104f | 再由核心數、變體及 restart 編號派生 |

最多 4 個 run、4096 個提案，不是每核各 4096。每個 run 的 65,536 次前向節點移動之外，撤銷至多增加相同次數；Tensor／邊掃描、每 64 個實際評估後的帳本重建、排程與精算成本另計。這些次數不能換算成固定秒數。

能量為 `max(max L[c,p], B/60) + 0.03*B/60 + 0.01*Σ(L[c,p]^2/max(1,W[p]))`。`L` 以各運算 `max(1,cycles)` 累計，`W` 為各 Pipe 總工作量；`B` 按 Tensor 來源／目的核心集合去重，另計直接 Op 邊、輸入及輸出，但不含實際溢出與同步。能量上升以 `exp(-ΔE/T)` 接受，較好或相同則直接接受；此代理量不能代替官方 cycles。

群組與 writer 關係先做傳遞閉包。只能完整移動／交換，超過上限就跳過，不能拆開。群組僅約束同核，不要求連續執行；後續仍依原 DAG 排程。截止也只能發生在完整移動／撤銷事務之間。

## 前段工作量與精算上限

既有上限繼續生效，但現在還受共同 deadline 限制：

| 階段／參數 | 預設上限 | 補充 |
|---|---:|---|
| v8 溢出回饋 | 48 次 Step2 重播／12 次近似細估 | 全部起點共用，入口 trace 另計 |
| communication_refine_candidates | 6 | 搬運群組 shortlist；可對一個起點追加有界回饋 |
| v10_refine_candidates | 8 | v10 候選的近似細估 |
| v10_feedback_starts | 3 | 共用 48 次重播／12 次細估 |
| v10_exact_candidates | 3 | 原精算前綴，含本次入口 |
| v10_exact_pool_candidates | 6 | 已包含前述 3 個 |
| v10_timing_order_candidates | 4 | 固定精算勝者的時間線調序 |
| v10_exact_spill_candidates | 4 | 同一起點的溢出調序 |

v10 最多 `6+4+4=14` 次完整多核精算；新增貪心／SA 最多 `1+2*2+4=9` 次，兩段最多 23 次。名額經正式核心序列去重，期限也會截短，並不承諾用完。前段 Step2 重播及近似細估另外計數。

時間線候選的細項仍位於 `src/timing_feedback.hpp`：`previous_pipe_ops=4`、`closure_attempts=32`、`closure_nodes=64`、`closure_edges=2048`、`per_core_distance=256`。群組、近似估價、同步和容量規則未因限時而放寬。細項搜尋配額可在 `src/search_limits.hpp` 查閱；提高配額不保證提高分數。

## 截止、保底與診斷

每段保存本次已完成並接受的候選。截止不記為官方模型非法，也不讓部分估價取代完整方案。v10 在完整精算後立即保存勝者，避免最後診斷刷新超時時丟失改進。最終按 `(精確cycles,總COPY bytes)` 選優，cycles 優先，搬運可能增加。

最後入口即使已過可選搜尋截止點，也要完成一次完整官方規則精算。若無效，程式返回錯誤，不輸出非法 plan。這個版本不保證整套 v10.1 完成，也不保證與未限時版本相比不退步；只保護本次實際完成並接受的結果。

`budget_stats` 記錄每段的絕對期限、結束時間、耗時與截止狀態。`annealing_stats.candidate_seconds` 包含 `annealing_seconds`；`v10_stats.exact_seconds` 包含 `tail_seconds`，不可重複相加。限時可能略過近似診斷刷新，查看 `v10_stats.diagnostics_current` 與 `annealing_stats.diagnostics_refreshed`；最終精確 cycles／bytes 對應實際輸出，不以過時近似值代替。

## 本輪結果與保留證據

0.11.1 已完成 **40／40 官方評分**，完整生成時間本次全部低於 20 秒，平均 **9.148 秒**、最慢 **17.881 秒**；24 點低於 15 秒、16 點在 15～20 秒。相對未限時 v10.1，平均單核加速比 **1.949478× → 1.945363×（-0.2111%）**，cycles 為 5 點改善、23 點相同、12 點退步。限時版沒有承諾保留未限時完整搜尋的品質，正式報告列出全部退步。

總 COPY +0.3885%、溢出 COPY -0.5424%、跨核分配搬運 +2.9937%。同次內部精算進度的平均加速比是限時前段 1.941464×、貪心 1.942375×、SA 最終 1.945363×，SA 相對貪心改善 4 點；這不是獨立官方消融。SA 本體平均 0.024 秒，含候選精算的整個新增階段平均 1.397 秒，不能相加。

數值由 `scripts/run_v11_budget.py`、`scripts/summarize_v11_budget.py` 產生，見 [官方逐點表](reports/budget20/RESULTS_V11_BUDGET20_zh.md) 與 [JSON](reports/budget20/RESULTS_V11_BUDGET20.json)。最終 cycles／總搬運 40／40 與官方一致；本輪完整差分 **144／144 一致、0 失敗**，其中 **16 組為雙方一致拒絕，不算合法方案**。兩編譯器各 21 項 CTest、CLI／26 項圖契約及 64 組合成圖與重複合法性核對通過，見 [驗證紀錄](reports/budget20/VALIDATION_V11_BUDGET20.json) 與 [來源保存清單](reports/budget20/SOURCE_V11_BUDGET20.json)。此時間結果只代表本機本次測試，不把 20 秒目標升格成任意輸入硬保證。

0.11.0 上一輪只有 20 組未限時貪心的 C++ 內部精算、4 組 baseline 外部官方驗證，尚無公開 SA 完整結果；見 [保留的部分結果](reports/PARTIAL_UNLIMITED_V11_zh.md)。v10.1 歷史平均單核加速比 1.949478×、平均生成 9.982 秒，仍位於同層 v10 目錄。這些歷史數字不是本輪限時版結果。

新源碼包為 `dist/dag_chain_solver_v11_1.zip`，舊執行檔、原始結果與來源保存清單不覆寫。
