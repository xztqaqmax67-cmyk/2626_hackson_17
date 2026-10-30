# DAG 鏈剖分排程器 v3

版本 **0.3.0**。本版在 v2 的候選搜尋外層加入多執行緒，用來縮短產生排程 JSON 的求解時間；候選種類、估價函數及選擇規則均沿用 v2。原有 `dag_chain_solver_v2` 保留在研究目錄中。

**本版尚未評分，等待官方評分器。** 下述開發檢查只驗證程式行為、輸出結構與一致性，不代表排程得分、官方有效性或加速比。多執行緒發生在求解器內，與輸出方案使用幾個 NPU 核心是不同概念。

## 演算法流程

1. 讀取圖與 Tensor，建立依賴、拓撲順序和靜態關鍵路徑估計。輸入中的 `COPY_IN`、`COPY_OUT` 不直接分配到輸出子圖，但其資料關係仍參與分析。
2. 根據前驅完成時間與通訊代價選擇鏈接，形成 DAG 鏈；再依工作量拆分長鏈。同一 Tensor 的多個寫入者採取同核配置策略。
3. 將鏈段分配到核心，平衡各 Pipe 負載，並考慮跨核搬運、同步及記憶體壓力。對固定核心配置生成可交錯執行的操作順序，每個原始計算操作對應一個輸出子圖。
4. 搜尋多組鏈參數、基礎貪心方案與有限次局部遷移，保留單核退路。若部分寫入的 Tensor 可能被換出，嘗試調整寫入者及其前置操作的順序。
5. 以沿用 v2 的內部估價比較候選：Pipe 時間草圖、正常搬運量、最晚下次使用原則下的溢出搬運估計，以及跨核等待。這些是搜尋啟發式，並非官方事件模擬或正式分數。
6. 合併各有效核心數的最佳候選，執行內部結構檢查，輸出 `node_to_subgraph` 與 `core_schedules`。較少核心的方案須通過 v2 原有的保守比較條件。

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

固定介面為 `npu_sched <graph.json> -n <2..5> -o <plan.json>`。核心數預設為 4；省略輸出路徑時，在輸入檔旁產生 `<輸入檔名>_multicore_res.json`。可用 `--help` 查看參數，`--version` 查看版本。正常執行只產生排程 JSON，不列印評分。

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
python tests/check_outputs.py --reference /path/to/v2/npu_sched --solver build-dev/bin/npu_sched --data /path/to/graphs --out runs/output_checks_v3 --cores 2 3 4 5
```

Windows 使用對應的 `.exe` 路徑；MSVC 初次設定時可加 `-A x64`。`contracts.py` 檢查 CLI、輸出與錯誤處理；`check_outputs.py` 重新產生 v2、v3 排程，檢查 JSON 結構、與 v2 逐位元組相同，以及重複執行的一致性。它們均不呼叫評分器。檢查腳本記錄的產生時間是求解器執行時間，不是方案的 NPU 執行時間或正式加速比。

編譯與檢查結果另記錄於研究目錄中的 `NON_SCORING_CHECKS.md`；`CHANGES_v3.md` 提供修改說明。這兩份補充文件不是編譯或執行的必要依賴。

## 原始碼提交包

在完整研究目錄執行：

```sh
python scripts/package.py --team dag_chain_solver_v3
```

產生 `dist/dag_chain_solver_v3.zip`，僅包含 `CMakeLists.txt`、本 README 與 `src/`。不包含資料、評分器、結果、測試腳本或建置產物。解壓後可直接依上述建置方式編譯；提交包採預設設定，無需開啟 `CHAIN_BUILD_TESTS`。
