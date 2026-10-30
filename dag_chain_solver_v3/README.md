# DAG 鏈剖分排程器 v3

版本 **0.3.1**。本版保留 v3 的多執行緒候選搜尋，並依新提供的官方題目包修正溢出估價與輸入、輸出契約。原有 `dag_chain_solver_v2` 保留；v0.3.0 已另存快照，便於比較。

**已使用附帶官方評分器完成問題 2／場景 B 的 40 組評估，40/40 通過。** 單核基準、平均加速比及逐例數值見研究目錄中的 `OFFICIAL_EVALUATION_v3_1.md`；這是公開用例測試，不代表隱藏用例或最終排行榜成績。多執行緒發生在求解器內，與輸出方案使用幾個 NPU 核心是不同概念。

## 演算法流程

1. 讀取圖與 Tensor，建立依賴、拓撲順序和靜態關鍵路徑估計。輸入中的 `COPY_IN`、`COPY_OUT` 不直接分配到輸出子圖，但其資料關係仍參與分析。
2. 根據前驅完成時間與通訊代價選擇鏈接，形成 DAG 鏈；再依工作量拆分長鏈。同一 Tensor 的多個計算寫入者必須同核，這是新版官方方案契約。
3. 將鏈段分配到核心，平衡各 Pipe 負載，並考慮跨核搬運、同步及記憶體壓力。對固定核心配置生成可交錯執行的操作順序，每個原始計算操作對應一個輸出子圖。
4. 搜尋多組鏈參數、基礎貪心方案與有限次局部遷移，保留單核退路。若部分寫入的 Tensor 可能被換出，額外嘗試調整寫入者及其前置操作的順序以減少搬運；原候選仍保留，不再僅因此現象直接拒絕。
5. 以內部估價比較候選：Pipe 時間草圖、正常搬運量、最晚下次使用原則下的溢出搬運估計，以及跨核等待。v0.3.1 更新溢出模型，這些仍是搜尋啟發式，並非官方事件模擬或正式分數。
6. 合併各有效核心數的最佳候選，執行內部結構檢查，輸出 `node_to_subgraph` 與 `core_schedules`。較少核心的方案須通過 v2 原有的保守比較條件。

溢出估價使用 `soft = floor(hard × 90%)`。每個操作先配置全部輸入、輸出並保護其使用中的 Tensor，再挑選可換出的資料；若沒有合法 victim，只要實際需求不超過硬容量，允許暫時超過軟門檻。Tensor 再次被寫入會使 DDR 備份失效，後續需要時重新寫回。這版也新增重複邊拒絕，以及最終方案的多寫者同核檢查。

固定硬體參數為 L1 `1572864` bytes、UB `131072` bytes、DDR `60` bytes/cycle、跨核 COPY 等待 `1000` cycles，依據新包 `config.txt` 與既有 C++ 提交要求。新包部分說明文件仍殘留 L1 `524288`、等待 `500` 的舊值，此處不採用。求解器不讀取配置檔，也不擴充至題目 1 或題目 3 的 Cache 模型。

目前未移植官方 Step3 的虛擬位元組額度 WAR/WAW 依賴，以及合併 Pipe FIFO、跨核 COPY 後的精確全域等待環檢查；內部估價與結構檢查無法替代官方完整驗證。

## v3 的平行化方式

使用者要求 `N` 個核心時，外層分別搜尋有效核心數 `2..N`，各自呼叫 `solve_at_core_count`。正常啟動時共使用 `N-1` 個計算執行緒，**包含主執行緒且最多 4 個**；`N=2` 時只有主執行緒，`N=5` 時新增 3 個工作執行緒。每個搜尋內部維持原來的順序執行。

- 所有工作共用唯讀的 `const Graph`，候選及中間狀態由各工作自行持有。
- 每個工作只寫入自己固定的結果槽位，不依完成先後更新全域最佳解。
- 全部工作 `join` 後，按原本由較大到較小有效核心數的次序比較結果，保留 v2 的平手處理與選擇順序。
- 工作例外會先收集，等所有執行緒結束後再按固定索引傳遞；執行緒建立失敗時該工作回退為同步執行。多份搜尋狀態同時存在，尖峰記憶體可能增加。

實作使用 C++17 的 `std::thread`，不引入第三方函式庫；UNIX 建置直接加入 `-pthread`，不使用 `find_package`。沒有新增執行緒數、隨機種子或搜尋時間限制的命令列選項。

## 建置與執行

需要 CMake 3.16 以上及支援 C++17 的編譯器。以下命令均在本目錄執行。

Linux：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched graph.json -n 4 -o plan.json
```

Windows（Visual Studio x64）：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe graph.json -n 4 -o plan.json
```

固定介面為 `npu_sched <graph.json> -n <2..5> -o <plan.json>`。核心數預設為 4；省略輸出路徑時，在輸入檔旁產生 `<輸入檔名>_multicore_res.json`。`--version` 顯示版本；不提供 `--help` 或 `--config`。正常執行只產生排程 JSON，不列印評分。

## 原始碼結構

| 檔案 | 用途 |
| --- | --- |
| `src/main.cpp` | CLI、圖輸入、錯誤處理與輸出寫入 |
| `src/model.hpp` | 圖、Tensor、排程資料結構及基礎工具 |
| `src/chain.hpp` | 求解器公開介面 |
| `src/chain.cpp` | 鏈剖分、核心配置、順序修復、估價與候選選擇 |
| `src/parallel_jobs.hpp` | 有界獨立工作執行、例外收集與執行緒回收 |

## 開發檢查（研究目錄限定，不執行評分）

完整研究目錄另外提供 `tests/` 與 `scripts/`，不屬於最小提交包。開啟 `CHAIN_BUILD_TESTS` 可建置結構不變量與執行緒行為檢查；Python 檢查腳本需 Python 3。

```sh
cmake -S . -B build-dev -DCMAKE_BUILD_TYPE=Release -DCHAIN_BUILD_TESTS=ON
cmake --build build-dev --config Release -j
ctest --test-dir build-dev -C Release --output-on-failure
python tests/contracts.py --solver build-dev/bin/npu_sched --out runs/contracts_v3
python tests/rules_contracts.py --solver build-dev/bin/npu_sched --out runs/rules_contracts_v3_1
python tests/check_rules_outputs.py --solver build-dev/bin/npu_sched --data /path/to/graphs --out runs/output_checks_v3_1
```

Windows 使用對應的 `.exe` 路徑；MSVC 初次設定時可加 `-A x64`。`contracts.py` 檢查 CLI、輸出與錯誤處理；`rules_contracts.py` 補上 Pipe、共享 ID、重複鍵／邊、含 COPY 的原圖環等契約案例。`check_rules_outputs.py` 產生方案並檢查輸出與重複執行一致性，可加 `--previous-output-dir /path/to/previous_outputs` 記錄與舊結果的差異，**不要求新版與舊版逐位元組相同**。上述檢查均不執行評分；產生 JSON 的耗時也不等於方案的 NPU 執行時間。

研究目錄中的 `RULES_UPDATE_v3_1.md` 說明本次規則對齊；本版編譯及檢查結果記錄於 `NON_SCORING_CHECKS_v3_1.md`。`CHANGES_v3.md`、`NON_SCORING_CHECKS.md` 及 `reports/validation_20261030.json` 僅代表 v0.3.0 的歷史狀態，`tests/check_outputs.py` 的舊版逐位元組比較亦僅適用於當時的多執行緒改造。這些補充文件與開發工具不是編譯或執行提交包的必要依賴。

## 官方評估（研究目錄限定）

使用者提供的完整題目包在 `references/hackathon_20261030/`；評估時保持其中程式、資料和配置不變。
`scripts/run_official_problem2.py` 評估已記錄並核對雜湊的 40 組 v0.3.1 方案，
`scripts/run_official_singlecore.py` 執行同包的單核基準，`scripts/summarize_official.py` 彙總已完成的結果。
詳細結果與原始產物位置見 `OFFICIAL_EVALUATION_v3_1.md`。
此前的 `NON_SCORING_CHECKS_v3_1.md` 與 `RULES_UPDATE_v3_1.md` 保留改版階段尚未評分時的紀錄。

## 原始碼提交包

在完整研究目錄執行：

```sh
python scripts/package.py --team dag_chain_solver_v3
```

產生 `dist/dag_chain_solver_v3.zip`，僅包含 `CMakeLists.txt`、本 README 與 `src/`。不包含 `references/`、附帶評分器、資料、結果、測試腳本或建置產物。解壓後可直接依上述建置方式編譯；提交包採預設設定，無需開啟 `CHAIN_BUILD_TESTS`。
