# DAG Chain Solver v7（0.7.0）

本版從 v6 0.6.1 複製，保留比較鍵、Tensor 使用快取等實作效率優化，預設取消限時追加搜尋，恢復 v5 的固定候選數／輪數。另加少量、固定配額的溢出搬運優化候選；v5、v6 目錄保留原樣。

## 建置與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 執行檔加 `.exe`。正式介面支援 2～5 核，預設 4 核；核心數是可用上限，允許空核。`--version` 顯示 `dag-chain-solver 0.7.0`。輸出只有 `node_to_subgraph`、`core_schedules`。求解器不讀官方評分結果、不啟動 Python、不識別公開用例名稱。

返回碼：0 成功、1 非預期錯誤、2 CLI 錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用方案。失敗保留既有輸出，拒絕覆蓋輸入。

## 演算法流程

1. 讀取完整 DAG，建立依賴、Tensor 使用快取及必須同核的多 writer 群組。
2. 生成鏈剖分與分配候選，依 v5 固定上限進行局部移動、記憶體排序、分區與 slack 搜尋。獨立核心預算並行執行，最多 4 個工作，最後按固定順序收集。
3. 保留完整 v5 搜尋的細化估價最優方案，以它為起點建立新的溢出優化候選。候選只改核心分配及拓撲順序，保留原 DAG 每條邊與 writer 同核條件。
4. 對候選重新粗估並以共享 DDR 事件模型細化；依估計 cycles、總搬運量及固定的平手順序選優。舊方案保留，粗估剪枝不會刪掉它。
5. 合併 2 到指定核心上限的方案，驗證結構後寫出調度 JSON。官方評分另行執行。

新增候選的機制及限制見 [v7 修改說明](CHANGES_v7_zh.md)。內部細化仍是近似模型，內部改善不保證官方 cycles 或搬運量同步改善，應閱讀完整測試結果。

## 搜尋上限

集中於 [src/search_limits.hpp](src/search_limits.hpp)，修改後重新編譯。

- `enable_timed_search=false`：預設固定工作量，完成即返回，不補時間。
- `seed_*`、`slack_*`：v5 原有候選數、輪數與圖規模上限。
- `spill_*`：本版新增溢出候選的有限配額。
- 若將 `enable_timed_search` 改為 `true`，才啟用保留的 v6 限時延伸；此時 `total_seconds` 才生效。

**想改成約 30 秒搜尋**，修改頭檔中原有的兩行，再重新編譯：

```cpp
inline constexpr bool enable_timed_search = true;
inline constexpr double total_seconds = 30.0; // 約 15 秒則改為 15.0。
```

只改秒數而不打開開關不會生效。限時預算包含完整 v7 固定搜尋，所有 worker 共用；固定階段不被中斷，因此不是任意圖的硬截止時間。

**想維持固定模式、只加強溢出搜尋**，可同步調整 `spill_move_evaluations`、`spill_move_keep`、`spill_max_candidates`。完整的修改示例、預設值、配額相互限制、重新編譯及評分方式見 [如何調整搜尋上限](SEARCH_LIMITS_zh.md)。文件中的加大示例尚未套用，預設仍為已測試的固定搜尋版。

## 規則與評測

容量、90% 軟閾值、共享 DDR 60 bytes/cycle、跨核延遲 1000 cycles 均沿用題目附件。官方 Step 2 決定換出對象與 SPILL 插入；本版透過排序與分配間接減少觸發溢出，不改官方換出策略。

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/contracts.py --solver build/bin/npu_sched --out runs/cli
python tests/rules_contracts.py --solver build/bin/npu_sched --out runs/rules
python tests/run_v5_synthetic.py --solver build/bin/npu_sched --out runs/synthetic
python scripts/run_v7_official.py --solver build/bin/npu_sched
python scripts/summarize_v7.py --solver build/bin/npu_sched
```

正式版與 `npu_sched_quick` 在預設固定搜尋模式下執行同一搜尋；只有開啟限時延伸後，quick 版才用較短預算。合成圖腳本名稱沿用，但測試本版程式。全部 40 組公開評測串行生成方案，再以原版評分器評估；輸出包含方案、官方結果、trace、log 與 SHA256。

v6 的歷史 15／30 秒工具與 `CHANGES_v6_zh.md` 是來源版本的留存資料；本版結果以新 `run_v7_official.py`、`summarize_v7.py` 為準。保存的 `reports/v5_results_baseline.json` 為歷史 v5 比較資料，單核 cycles 來自新版原評分器。

`python scripts/package.py` 產生 `dist/dag_chain_solver_v7.zip`，僅含正式建置所需源碼與說明，不含用例、評分器、測試資料或二進位。原版本及規則檔案的保留檢查記錄見 reports 下的 manifest。

## 本次驗證與結果

MSVC 19.44、GCC/MinGW 15.1 各通過 10/10 組 C++ 測試；正式 MSVC 執行檔通過 CLI 契約、26/26 圖輸入契約，以及 64/64 組合成圖官方評分。64 組合成圖均重複生成並確認方案逐位元組相同。未測試原生 Linux 或較舊的 GCC。

全部 40 組公開方案通過官方評分。與歷史 v5 相比，cycles 為 16 組改善、24 組持平、0 組退步，cycles 合計減少 0.783%。總 COPY 減少 0.089%，溢出 COPY 只減少 0.038%；後者有 9 組減少、25 組持平、6 組增加，不能稱為大幅降低溢出的版本。

五核平均單核加速比由 v5 的 2.195383× 增至 2.231997×。與 v6 約 15 秒結果相比，五核本次平均生成時間 4.555 秒（v6 歷史為 14.939 秒），但 cycles 合計增加 0.984%、溢出 COPY 增加 16.528%。縮短搜尋有品質取捨，並非全面超越 v6。

[全部 40 組表格及比較](reports/RESULTS_2_TO_5_CORES_zh.md) · [精確 JSON 與檔案路徑](reports/RESULTS_2_TO_5_CORES.json) · [驗證紀錄](reports/VALIDATION_V7.json)

## 上傳 GitHub 時的目錄

建議上傳完整 v7 開發目錄並沿用 `.gitignore`：保留 `src/`、`tests/`、`scripts/`、`CMakeLists.txt`、說明文件和所需 `reports/`；`build*/`、`runs/`、`dist/`、`references/`、`snapshots/` 及 Python 快取已排除。調參說明位於根目錄 `SEARCH_LIMITS_zh.md`，其他 AI 或開發者可直接由本 README 進入。

GitHub 上不含官方附件時，要自行提供相同版本的 `hackathon.zip`，再執行 `python scripts/prepare_reference.py --archive /path/to/hackathon.zip`；腳本會依 manifest 核對並還原本地評分檔案。報告中的 raw result／trace 與歷史 v5、v6 比較路徑是本地實驗證據，僅上傳本目錄不會附帶這些外部資料。

`dist/dag_chain_solver_v7.zip` 是精簡的正式原始碼包，包含 src、CMake 與說明；它可獨立建置求解器，但不包含開發測試／評測腳本／結果報告。完整開發協作請使用目錄內容，而非把這個 zip 當成整個開發倉庫。
