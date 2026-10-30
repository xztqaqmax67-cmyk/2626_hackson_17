# 0.6.1 搜尋上限與時間預算

主要設定集中於 [src/search_limits.hpp](src/search_limits.hpp)。改完必須重新編譯，
正式 CLI 不接受額外調參旗標。這些設定改變搜尋工作量，不改變題目規則。

## 先分清三種設定

| 類別 | 所在位置 | 用途 |
|---|---|---|
| 搜尋上限 | `src/search_limits.hpp` | 時間、候選池、每輪粗估／細化數、圖規模門檻。 |
| 候選策略與參數組合 | `src/anytime.cpp`、`src/chain.cpp`、`src/regions.cpp` | 候選類型、排序模式、權重陣列、分層方式；不是只改一個上限即可增加全部策略。 |
| 題目規則 | `src/model.hpp` 的 `Config`，來源是題目 config | DDR 60、跨核等待 1000、L1／UB 容量和 90% 軟門檻。不可拿來調快成績。 |

## 整體時間

| 參數 | 現值 | 含義與調整影響 |
|---|---:|---|
| `total_seconds` | 15.0 | 正式生成的共用牆鐘預算；最直接調整目標時間的位置。 |
| `quick_test_seconds` | 0.50 | `npu_sched_quick` 專用，正式 `npu_sched` 不使用。 |
| `timed_reserve_seconds` | 0.15 | 預留給結束、驗證與寫檔；追加搜尋截止點約為起點後 14.85 秒。 |
| `timed_workers` | 4 | 追加搜尋的工作數，包含呼叫執行緒；helper 支援 1～4。 |

時間從讀圖前起算，種子搜尋也占用這 15 秒，不是「原 v5 跑完再加 15 秒」。
例如種子耗時約 8 秒，追加搜尋通常只有約 7 秒可用。四個 worker 共用同一截止點；
CPU 累計工作時間可能大於 15 秒，這不代表牆鐘預算乘了四倍。

種子必須先完整完成。如果種子、輸入解析或不可立即取消的工作已超出目標，不能保證任意圖都準時返回。
新的粗估／事件細化會定期檢查 `Deadline` 並拋出 `SearchExpired`，只丟棄正在處理的候選，
不覆蓋之前已完成的最佳方案。最後仍需等待執行緒結束並寫檔。

如果某個保留候選池的全部鄰域均已探索完，或去重記憶已達上限，程式可提前結束。
沒有睡眠或空轉填滿預算。調大時間不保證官方 cycles 更低。

## 追加搜尋寬度

以下上限按每個獨立搜尋工作計算：

| 參數 | 現值 | 含義與取捨 |
|---|---:|---|
| `timed_beam` | 6 | 保留的已細化搜尋起點數。增加可探索較多方向，但增加記憶體與每輪工作。最佳已完成方案另外保存。 |
| `timed_coarse_per_round` | 12 | 每輪收集的非重複、記憶體粗估可用候選目標上限；無效／重複嘗試另有每輪 8 倍嘗試限額。 |
| `timed_refine_per_round` | 3 | 每輪進入共享 DDR 事件細化的候選數上限。增加會減少可完成的輪數。 |
| `timed_seen_limit` | 500000 | 固定大小 128-bit 去重簽章的數量上限，避免記憶體無界增長；達上限的工作會停止。 |

`timed_refine_per_round` 大於粗估候選數不會憑空增加候選。每輪未達 12 個也可進入細化，
候選被去重、容量粗估拒絕或鄰域耗盡時，實際工作數會更少。

去重簽章是固定雙雜湊，不是亂數。理論上的碰撞最多略過候選，不會使候選跳過合法性檢查。
候選是否合法仍取決於完整原圖、writer、容量與事件檢查，而非雜湊。

## 原 v5 種子上限

為保留原 v5 起點，下列數值未提高。增大它們會占用更多共同預算，使追加搜尋時間變少。
`graph_work = 計算 Op 數 + 邊數 + Tensor 數`，由輸入大小決定，不讀用例名稱。

| 參數 | 現值 | 說明 |
|---|---:|---|
| `medium_graph_work` | 80000 | 超過時縮減種子變體、區域與 slack 搜尋。 |
| `large_graph_work` | 200000 | 超過時進一步縮減種子，停用部分新增局部搜尋。 |
| `seed_variants_small / medium / large` | 10 / 6 / 2 | 鏈分配參數組數；不得超過 `chain.cpp` 的 10 組變體陣列。 |
| `seed_pool_limit` | 24 | 粗候選池觸發裁剪的門檻；另有保留舊搜尋入圍方案的流程。 |
| `seed_keep_time` | 10 | 裁剪時按時間取向保留的配額。 |
| `seed_keep_traffic` | 6 | 按搬運量取向保留的配額。 |
| `seed_keep_other` | 4 | 另外兩種取向各保留的配額。 |
| `seed_legacy_keep` | 4 | 舊搜尋路徑保留數。 |
| `seed_writer_repair_passes` | 2 | 寫入者順序修復的最大輪數。 |
| `seed_legacy_rounds / seed_local_rounds` | 2 / 3 | 舊段搬移與目前局部搜尋的最大輪數。 |
| `seed_group_limit` | 24 | 每輪挑選的較重鏈段數上限。 |
| `seed_tensor_limit / seed_writer_limit` | 16 / 16 | Tensor 使用者鄰域與 writer 群組的數量上限。 |
| `seed_swap_group_limit` | 8 | 用於兩段交換的群組範圍上限，交換組合數隨此值增加。 |
| `seed_legacy_move_limit / seed_local_move_limit` | 4 / 6 | 每輪進一步評估的搬移候選上限。 |
| `seed_reorder_bases` | 3 | 嘗試額外記憶體導向重排的種子數上限。 |

`seed_pool_limit` 是裁剪觸發門檻，不是修改保留配額後仍可保證的硬上限。
只增加它不等於保留同樣多的候選：裁剪實際由各 `seed_keep_*` 配額共同決定。
目前配額合計為 10+6+4+4，去重後可能更少；另保留的舊搜尋方案也可能使細化數超過 24。

## 原 v5 的 slack 候選

| 參數 | 現值 | 含義 |
|---|---:|---|
| `slack_groups_small / medium` | 24 / 8 | 有消費者等待餘量的 writer 群組篩選上限。 |
| `slack_replays_small / medium` | 8 / 4 | 完整 Placement 重播次數上限。 |
| `slack_assignments` | 2 | 保留的分配數，每種再試 critical／DFS 順序，通常最多 4 個輸出。 |

這些上限不表示可免費等待若干 cycles。判斷仍針對受影響消費者，跨核資料須完成 COPY_OUT、
1000 cycles 等待與 COPY_IN；共享 DDR 及 spill 仍須進一步評估。

## 不在上限表中的策略組合

[src/anytime.cpp](src/anytime.cpp) 的 `order_options`、`proposal` 和 `Neighborhoods::counts`
定義八類候選及枚舉方式。固定分配重排使用 critical／DFS／breadth／ID 優先序、
記憶體權重 `0、0.125、0.25、0.5、1、2、4、8` 與 ready 視窗 `1、4、8、16、32`。
這一分支的計數為 `4 × 8 × 5 = 160`。新的自由分配候選使用四種排序模式、六個負載權重、
六個通信權重和四個視窗，計數為 `4 × 6 × 6 × 4 = 576`；權重均為
`0、0.125、0.25、0.5、1、2`，視窗使用上述陣列的前四個值。
ready 優先序旋轉仍透過拓撲 ready queue 產生順序。
更改這些陣列時需同步更新 `counts` 中的 160／576、各除數及索引解碼，
不應只增加數字而讓索引超出陣列或部分組合永遠不被枚舉。

[src/regions.cpp](src/regions.cpp) 保留 2／4／8 個加權深度區間和有界分叉／匯合切點策略。
分區配額枚舉考慮 1+4、2+3 等正整數配額與實體核心集合，不把每塊最多三核當作題目限制。
這些是分區策略選項，並不是 `timed_beam` 或 `seed_pool_limit` 的另一個名稱。

## 如何比較修改

每次只改通用策略／上限，重新編譯，使用新結果目錄保存執行檔與源碼雜湊。
採同一組輸入與未修改的官方評分器，比較實際生成秒數、正式 cycles、加速比和 COPY 分類。
計時版不要求重跑輸出完全一致；不能把不同次生成的時間、方案和分數拼成一筆。

`TimedSearchStats` 可在開發 API 中觀察 attempted、unique_candidates、refined、improvements、
base_seconds、elapsed_seconds、exhausted_lanes 等。`cancelled` 表示因截止時間結束的工作數，
可能是在兩個候選之間停止，不是精確的半成品候選數；`improvements` 只計嚴格改善細化 cycles／搬運的更新。
上述統計不是官方評分，正式 CLI 不會將它們混進提交 JSON。
