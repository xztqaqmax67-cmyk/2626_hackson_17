# DAG Chain Solver v8（0.8.0）

本版從 v7 複製，增加「先預演官方換出規則，再依溢出事件重排」的固定配額後處理。保留 v7 初始方案，核心分配不變，只調整核內的合法順序。v7 與更早版本保留原樣。

## 流程

1. 執行既有 v7 搜尋，得到完整可用方案。
2. C++ 重建官方 Scene B 的核內圖、邊界 COPY 及 Step 1 排序，執行與官方 Step 2 對齊的換出判定。
3. 記錄每次溢出的核心、Tensor、觸發位置、前後使用位置、是否需要寫回與重載字節數。
4. 按實際溢出記錄產生重排候選：將可合法提前的使用者／必要前驅前移，或延後導致壓力的任務。
5. 對候選重新執行 Step 2。只有溢出搬運嚴格減少、分核搬運不變，且舊細化模型預測 cycles 不增加的候選，才可取代原方案。
6. 接受候選後重新取得溢出記錄，最多進行固定輪數，最後輸出原有調度 JSON。

「相同邏輯」的範圍是核內圖生成、Step 1／子圖優先序及 Step 2 的溢出判定與記錄。並未宣稱完整官方 Step 3 cycles 模擬已全部移植；`refine.cpp` 仍是既有 cycles 估價器。因此 cycles 的內部非退步檢查不等於官方分數保證，最終使用未修改的官方評分器驗證。

正式執行檔只執行 C++，不呼叫 Python、不讀官方答案或歷史結果。這裡的預演是計算模擬，不會真的搬動 Tensor 資料；預演耗時屬於本電腦產生方案的時間。

## 建置與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 執行檔加 `.exe`。核心上限 2～5，預設 4；允許空核。正式 CLI 與輸出格式不變。`--version` 顯示 `dag-chain-solver 0.8.0`。

## 可調上限

集中於 `src/search_limits.hpp`，修改後重新編譯：

| 參數 | 預設 | 意義 |
|---|---:|---|
| enable_timed_search | false | 前段仍使用 v7 固定搜尋量 |
| feedback_enabled | true | 開啟新溢出回饋；false 可直接使用 v7 搜尋結果 |
| feedback_rounds | 2 | 最多重排輪數 |
| feedback_records | 8 | 每輪優先處理的溢出記錄 |
| feedback_candidates_per_round | 12 | 每輪完整 Step 2 重播的候選配額 |
| feedback_refine_candidates | 4 | 每輪 cycles 細化配額 |
| feedback_closure_nodes | 64 | 合法前移所需前驅閉包的節點上限 |
| feedback_max_distance | 256 | 全域拓撲序中局部調整的距離上限 |

其他 seed、slack、spill 配額沿用 v7。若啟用可選限時搜尋，`total_seconds` 管前段搜尋，新回饋階段另用固定配額，總生成時間可能更長。詳細設定與編譯方式見 [搜尋上限說明](SEARCH_LIMITS_zh.md)。

## 開發與驗證

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python scripts/run_v8_official.py --solver build/bin/npu_sched
```

差分測試逐筆核對 C++ 與官方 Python 的核內序列、換出 Tensor、前後使用位置與實際換出／重載字節數。Python 只用於開發時核對，正式程式沒有該依賴。公開用例不是唯一驗證來源；另使用包含平手、不同 ID／輸入順序、多 writer 和 backing 重用的合成圖。

`references/` 保留原版評分器與附件，禁止修改其規則；上傳 GitHub 時沿用 `.gitignore`。`scripts/package.py` 產生精簡源碼包，不含用例、評分器、二進位或開發測試。

## 查看每一筆溢出

開發建置會產生 `spill_diagnostics`。以下從 v8 目錄執行，Windows 使用 `.exe`：

```powershell
./build_msvc/bin/spill_diagnostics.exe input.json plan.json trace.json
./build_msvc/bin/spill_diagnostics.exe input.json plan.json adjusted_trace.json --improve adjusted_plan.json
```

第一行只預演已有方案，第二行另外嘗試事件回饋重排。JSON 包含 `cores[].sequence` 與 `records[]`：每筆記錄有核心、Tensor ID、觸發／上次／下次使用位置、COPY ID，以及 `spill_out_copies_data`。實際溢出字節數為 `size * (1 + spill_out_copies_data)`；有有效且乾淨 DDR 副本時只需重載。`cores[].peak` 是 Step 2 換出後的邏輯駐留峰值，不是 Step 3 的並行執行峰值。

診斷工具接受本求解器產生的單節點子圖方案，並沿用同 Tensor writer 同核條件；它不是可讀任意子圖分區格式的通用官方評分器替代品。原運算 owner 欄位為 C++ Graph 的密集索引，`trigger_op`／`prev_use_op`／`next_use_op` 才是實際操作 ID。生成 ID 或字節數超出 C++ 有號 64 位範圍時，預演會明確回報失敗，回饋階段保留初始方案。

## 本次實測

MSVC 19.44 與 GCC/MinGW 15.1 各通過 11 組 C++ 測試；CLI、26 組圖輸入契約、64 組合成圖官方評分及重複輸出一致性也通過。120 組初始逐筆差分全部一致；新增 6 個重排方案再逐筆核對一致，另 34 個最終方案與已驗證方案的輸入／方案／程式雜湊完全相同。

全部 40 組公開官方評分通過。相對 v7：核心分配與分核搬運均不變；溢出 6 組下降、34 組持平，合計減少 2,550,400 bytes（0.06053%）；cycles 5 組下降、35 組持平，合計減少 0.02562%。平均加速比 1.933066× → 1.933303×，本輪效果仍小。

[完整 40 組結果](reports/RESULTS_V8_zh.md) · [溢出記錄解讀示例](reports/SPILL_TRACE_EXAMPLE_zh.md) · [驗證紀錄](reports/VALIDATION_V8.json)

目前回饋策略有小配額、全域移動距離與必須立即減少溢出的限制。這版建立可核對的事件回饋流程，沒有宣稱已充分搜尋全部拓撲順序。未驗證原生 Linux 或更舊 GCC。
