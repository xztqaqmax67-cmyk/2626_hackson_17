# v9 搜尋上限與分組參數

所有參數位於 `src/search_limits.hpp`，修改後重新編譯。這些是搜尋參數，不是官方規則。正式 CLI 沒有額外調參旗標。

## 新增搬運分組階段

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

預設 `enable_timed_search=false`，固定工作量完成即返回。設true並修改total_seconds才會啟用前段追加搜尋；它不包含後續v8回饋與v9分組階段，因此不是整個程式的硬截止。`-n`是NPU核心上限，`timed_workers`是前段追加搜尋執行緒，建置`--parallel`是編譯工作數，三者不同。

所有正式報告使用預設固定模式，生成時間不含官方評分。測試工具90秒watchdog是測試超時，不是搜尋上限。使用新目錄保存調參實驗，原始比較結果不要覆寫。
