# 模組化新版算法的 Scene B 提交工程

此目錄是可獨立建置的提交版本，依據提交要求 v3.0（2026-10-30）。只將本目錄的 `CMakeLists.txt`、`README.md`、`src/`、`third_party/` 放入實際隊伍編號的目錄，再壓成 `<隊伍編號>.zip`。上層研究工程、官方評分器副本、Python、資料、測試、預存方案、`build/` 與二進位檔均不屬於提交包。

## 建置與執行

Linux，GCC 9.4+，CMake 3.16+：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched "/path with spaces/graph.json" -n 4 -o "/path with spaces/plan.json"
```

Windows，Visual Studio 2022，CMake 3.20+：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe "C:\input with spaces\graph.json" -n 4 -o "C:\output with spaces\plan.json"
```

C++17，包含全部依賴源碼，不使用 `find_package` 或任何下載機制。產物直接位於 `build/bin/`，Windows 多配置也不增加 `Release/` 子目錄。支援中文及空格路徑，不依賴啟動時的工作目錄。

命令列：`npu_sched <graph.json> [-n N] [-o plan.json]`，長選項為 `--cores`、`--output`，同時支援空格及等號形式。核心數 2～5，預設 4。輸出預設在輸入目錄下，命名 `<stem>_multicore_res.json`，會建立輸出目錄。`--version` 顯示版本。

退出碼：0 成功；1 內部錯誤；2 參數錯誤；3 輸入錯誤；4 輸出錯誤；5 無通過檢查的候選。錯誤寫 stderr，成功時 stdout 為空。無 `--config`、時間、種子或評分選項，不讀取 `config.txt` 或環境變數設定。

## 參數與方案

固定 L1 = 1572864、UB = 131072 bytes、DDR = 60 bytes/cycle、跨核延遲 = 1000 cycles。COPY_OUT 完成後才開始計跨核等待。保留資料實際使用的獨立 `PIPE_FIX`，以及 COPY 的 `cycles=-1` 佔位值支援。

輸出只有 `node_to_subgraph` 和 `core_schedules`；全部非邊界 COPY 算子恰好分配一次，子圖編號為非負整數，各核心按全域子圖拓撲序抽取自己的隊列。所有寫入同一 Tensor 的算子會歸到同一核心，避免跨核心副本與本地寫回之間產生相反順序。

## 算法流程與研究版差異

1. 用 Tensor 的生產者／消費者關係建立 Op DAG，收縮邊界 COPY，計算關鍵路徑、拓撲順序及連通分支。
2. 保留單核心拓撲與 DFS 候選；產生關鍵路徑管道排程、結構分支、分層帶、分隔點及切塊放置等不同起點，沿用模組化研究版的 `core`、`pipeline`、`structure` 算法。
3. 統一多寫入 Tensor 的核心分配。以容量／下次使用的序列模型排除未完成所有寫入卻必須換出的候選，降低資源死鎖風險。
4. 內部以管道時序草圖、DDR 路由量和平均超容量生命週期面積排序，保留最多 8 個候選。這些數字僅用於搜尋，不輸出，也不等同正式分數。
5. 嘗試兩個最佳候選的同核重排、六個最重子圖的核心遷移，以及最大子圖的分裂。固定候選順序，最多 150000 個工作檢查點，以整數 ID 及方案內容確定平手順序。
6. 對最佳完整方案檢查覆蓋與拓撲後寫出；若無候選通過風險篩選，回傳 5。

研究版會以移植的官方評分器逐一評分、使用子程序限時、啟動多條候選生成執行緒、繼承檢查點；這些機制不納入提交版。提交版只保留自主算法與內部估價，單執行緒、無子程序、無網路、無檢查點，也不讀 stdin。它與先前 25 秒研究版的分數不能直接視為同一版本。

搜尋停止位置由固定工作量決定，兩次執行不會因牆鐘速度不同而選到不同候選。90 秒與容量合法性仍需實測；風險篩選不是官方評估的替代品，不能證明任意未知輸入都會成功。

## 第三方庫

| 名稱 | 版本 | 授權 | 用途 | 路徑 |
| --- | --- | --- | --- | --- |
| nlohmann/json | 3.11.3 | MIT，完整授權在 `third_party/LICENSE.json` | JSON 解析及序列化 | `third_party/json.hpp`，完整單頭檔發行版 |

無其他第三方庫。演算法及圖索引源碼均在 `src/`，未包含評分器實作。Windows UTF-8 命令列轉換以 `_WIN32` 隔離系統 API。

外部驗證腳本在倉庫的 `tools/`，不納入此提交包。完整測試、平台驗證範圍與已知限制見倉庫 `SUBMISSION_REVIEW_20261030.md`。本目錄獨立維護，上層研究版檔案仍可用於後續算法比較。
