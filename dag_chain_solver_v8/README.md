# DAG Chain Solver v8（0.8.1）

本版在 v8 目錄直接升級溢出回饋搜尋。修改前 0.8.0 的源碼、測試、說明、報告及主執行檔已保存至 `backups/v8_0_8_0_before_feedback_upgrade_20261031/snapshot.zip`，原始評測仍在 `runs/official_all/`。v7 與更早版本保留原樣。

## 演算法流程

1. 完整執行既有 v7 固定搜尋，保留原最佳方案，額外收集最多 3 個已細化評估、預估 cycles 不超過最優 1.05 倍的不同方案。核心標籤置換不算不同分配。
2. C++ 重建官方 Scene B 核內圖、邊界 COPY、Step 1 排序及 Step 2 換出事件，得到精確搬運量與 Tensor 前後使用位置。
3. 從原方案的溢出熱點產生最多 4 個移核候選。移動完整 writer 群組，保留拓撲順序；新增跨核搬運必須納入總量檢查。
4. 各狀態按核心、Tensor、觸發區域分散選取記錄；無法形成新合法候選時繼續遞補，最多掃描 64 筆、使用 8 筆有用記錄。
5. 交錯產生五種動作：提前下一使用者、提前剩餘使用者、延後壓力觸發者至下一／最後使用者之後，以及延後實際生產者。移動距離按核內步數計算，依然保留完整 DAG 前驅／後繼閉包。
6. 用寬度 3、深度 2 的有界 beam 搜尋，允許搬運量不變的中間步。每個候選重新預演，不能沿用舊 victim 記錄；按各核心實際序列去重。
7. 預篩交替考慮預估時間與精確總搬運量的非支配候選；細化後以 cycles 優先、總搬運量次之選優，最後驗證並輸出原有格式的方案 JSON。

所有起點、移核與兩步搜尋共用最多 48 次候選 Step 2 重播、12 次完整細化估時；初始方案的第一次 trace 另計。每個深度另有限額，不會將全部配額乘上起點數。

## 接受條件與規則範圍

最終方案的**精確總 COPY bytes 及估計 cycles 均不得高於回饋入口的原方案**，至少一項嚴格改善才替換。固定分配的重排另要求分核搬運相同、溢出不高於父狀態。不同分配比較總 COPY，不能只靠降低溢出掩蓋新增跨核搬運；溢出分類本身仍可能增加。

入口方案是本次前段搜尋的勝者，不是磁碟上的歷史 v8 0.8.0 結果；正式執行檔不讀歷史答案。修改前後的實際改善、持平、退步都保留在官方比較報告。

C++ 與官方對齊的範圍是本求解器單節點子圖表示的 Scene B 圖生成、Step 1 與 Step 2。**完整官方 Step 3 尚未移植，`refine.cpp` 的 cycles 仍是近似值。** 內部估時不退步不代表官方 cycles 保證；完整官方評分器用於開發驗證。本版不修改容量、90% 軟閾值、DDR 60 bytes/cycle 或 1000 cycles 同步延遲，不刪除原始依賴，也不依據用例名稱或既有答案選擇策略。

正式執行檔只使用 C++。預演是 CPU 模擬，沒有實際搬動 Tensor 資料；這部分耗時屬於本電腦生成方案的時間。

## 建置與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 執行檔加 `.exe`。核心上限 2～5、預設 4、允許空核；`--version` 顯示 `dag-chain-solver 0.8.1`。正式 CLI 與方案格式不變。診斷資訊以一行 `feedback_stats {JSON}` 寫至 stderr，不混入方案或 stdout。

## 搜尋配額與診斷

所有可調搜尋上限集中於 `src/search_limits.hpp`，修改後重新編譯。詳見 [SEARCH_LIMITS_zh.md](SEARCH_LIMITS_zh.md)。固定配額不等於固定秒數；預設不啟用 15 秒限時延伸。開啟限時模式時，回饋階段仍另計固定工作量，額外多起點收集僅在預設固定模式啟用。

`feedback_stats` 記錄掃描／有用事件數、各策略嘗試與生成數、實際重播與細化次數、中性狀態保留數、最終勝者經過的中性步數及拒絕原因。拒絕原因包括錨點無效、依賴、距離、閉包大小、重複、trace 失敗、搬運超限、估時無效及估計 cycles 變差。`neutral_retained` 計算 beam 選取時總搬運等於入口的狀態次數，包含初始為入口騰位而移除者；`best_neutral_steps` 才是最終勝者路徑的中性步數。`accepted_stages` 也包含起點評估階段。不同計數的單位不同，不能相加當作唯一候選總數。

## 開發驗證

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python scripts/run_v8_official.py --solver build/bin/npu_sched --out runs/official_upgrade
python scripts/summarize_v8_upgrade.py
```

完整目錄包含測試；精簡源碼包未包含 tests，使用預設 `CHAIN_BUILD_TESTS=OFF`。公開資料以外，合成測試涵蓋完整 DAG 合法性、無效事件遞補、跨核心距離、延後生產者、真正兩步中性搜尋、多 writer 整組移核、確定性及配額。差分核對 C++ 與官方 Python 每筆換出事件、COPY ID、backing 版本、核內序列和搬運字節數。

`references/` 是未修改的原版評分器與附件。`scripts/package.py` 生成源碼 ZIP；`.gitignore` 排除評測資料、建置、備份和壓縮包。舊版 `summarize_v8.py`／`check_final_spill.py` 是 0.8.0 的歷史工具，不用於新版資料；新版使用帶 upgrade 名稱的報告工具和新輸出目錄。

## 查看溢出事件

```powershell
./build_msvc/bin/spill_diagnostics.exe input.json plan.json trace.json
./build_msvc/bin/spill_diagnostics.exe input.json plan.json adjusted_trace.json --improve adjusted_plan.json
```

診斷工具接受本求解器的完整單節點子圖方案，沿用同 Tensor writer 同核條件。JSON `records[]` 包含核心、Tensor、前次／觸發／下次使用位置、COPY ID 和有效 DDR backing；每筆溢出 bytes = `size * (1 + spill_out_copies_data)`。`cores[].peak` 是 Step 2 換出後的邏輯峰值，不是 Step 3 並行執行峰值。外部方案會重建全域拓撲序，因此診斷重排不必與正式產生器隱含的全域序列完全相同。

## 本次結果

MSVC與MinGW各12/12 C++測試通過；CLI、26組輸入契約、64組合成圖官方評分及重複輸出一致均通過。使用新版二進位重新執行120組逐筆差分全部一致，其中包含40個最終正式方案；未沿用舊版二進位的核對結果。

本次40組官方評分均通過。相對0.8.0，平均加速比1.933303×→1.935286×（+0.10256%）；cycles合計減少43,478（−0.03052%），8組改善、24組持平、8組退步。總COPY減少642,816bytes（−0.00901%），溢出減少675,584bytes（−0.01604%）。平均生成時間4.215→5.336秒；前後為不同時間的單次量測，非排除環境波動的CPU實驗。收益仍小，額外搜尋有時間成本。

測試與比較記錄於 [完整 40 組結果](reports/RESULTS_V8_UPGRADE_zh.md)、[機器可讀結果](reports/RESULTS_V8_UPGRADE.json) 及 [驗證紀錄](reports/VALIDATION_V8_UPGRADE.json)。歷史 0.8.0 報告仍為 `reports/RESULTS_V8_zh.md`，不代表新版數值。

[回饋搜尋診斷解讀](reports/SEARCH_DIAGNOSTICS_V8_UPGRADE_zh.md)。報告、測試與原始trace存放在完整開發目錄；精簡源碼ZIP只包含建置所需源碼及三份說明。
