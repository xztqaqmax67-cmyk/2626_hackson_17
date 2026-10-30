# 舊版算法的 Scene B 提交工程

本目錄按 `材料/提交要求文档.md` v3.0（2026-10-30）整理，獨立可建置。提交時只複製本目錄的 `CMakeLists.txt`、`README.md`、`src/`，把外層目錄改成實際隊伍編號，再壓成同名 ZIP。不要打包上層研究用的 `score.py`、`local_eval/`、資料、報告或任何 `build/`。

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

兩種建置都使用 C++17，產物直接位於 `build/bin/`，多配置產生器也不會多加 `Release/` 一層。程式支援中文及空格路徑，可從其他工作目錄啟動，不尋找工程資源或設定檔。

命令列為 `npu_sched <graph.json> [-n N] [-o plan.json]`。`--cores=4`、`--cores 4`、`--output=path`、`--output path` 都可使用；核心數只接受 2～5，預設 4。省略輸出路徑時，寫入輸入同目錄下的 `<stem>_multicore_res.json`，自動建立需要的目錄。`--version` 顯示版本。禁止覆寫輸入圖。

退出碼：0 成功；1 內部錯誤；2 命令列錯誤；3 輸入錯誤；4 輸出錯誤；5 找不到通過方案檢查的候選。成功時 stdout 為空，錯誤寫 stderr。不接受 `--config`、`--time-limit` 或其他研究版參數。

## 固定硬體與輸出

L1 = 1572864 bytes，UB = 131072 bytes，DDR = 60 bytes/cycle，跨核等待 = 1000 cycles。數值寫死在程式內，不讀取設定檔或環境變數。同一 Tensor 的所有寫入者放在同一核心。跨核估價為 COPY_OUT 完成、等待 1000、COPY_IN、消費者執行。

輸出只有 `node_to_subgraph` 和 `core_schedules`。每個非 COPY_IN/COPY_OUT 算子獨立成一個子圖，以算子 ID 作為子圖 ID；各核心順序取自同一個全域拓撲序，允許空核心。保留資料中已使用的獨立 `PIPE_FIX` 支援；提交文件的管道欄位表漏列該名稱，題目備註及實際資料含有它。

## 算法流程

1. 讀取 Tensor–Op 二部 DAG，去除邊界 COPY 節點，建立計算算子的前驅、後繼、關鍵路徑長度、拓撲層及反向 DFS 順序。把同一 Tensor 的寫入者用並查集合併為同核群組。
2. 建立關鍵路徑、負載平衡、廣度、DFS、ID 順序等起始方案。每次從已就緒的算子中取固定大小的視窗，估算各核心五條管道的最早完成時間，決定核心與順序。
3. 以拓撲層帶和連通分支產生結構方案，對較好的核心分配再做減少記憶體重疊的重排；另保留全圖單核心的拓撲及 DFS 候選。
4. 內部估價結合管道時序草圖、DDR 路由量與最遠下次使用策略估計的溢出量。若必須在多寫入 Tensor 的最後一次寫入之前把它換出，或當前必要資料超過容量，對該候選給予不可行標記。這是保守篩選，並非官方評分器。
5. 固定最多 4 輪局部搜尋，嘗試 Tensor 使用者群、重算子遷移及固定種子交換；最多 4000 個確定性的內部檢查點。計數而非牆鐘時間決定停止位置，保留已完成候選並按固定順序決勝。
6. 檢查覆蓋範圍、核心及拓撲順序；若沒有通過記憶體風險篩選的候選，回傳 5，否則輸出最佳方案。

演算法單執行緒，不建立子程序、不讀 stdin、不連網、不呼叫評分器，也不輸出分數。固定工作量提供可重現輸出；90 秒限制需在驗收硬體及實際輸入上量測，不能由迴圈上限推導任意大小輸入的時間保證。

## 第三方庫與來源

無第三方庫。JSON 讀取器及算法均以本工程 C++17 源碼提供，僅依賴標準庫；Windows 命令列 UTF-8 轉換使用以 `_WIN32` 隔離的系統 API。算法源於本倉庫舊版 `hackathon_v3/solver.cpp`，參考原問題 2 的管道、分層及局部搜尋設計。

本提交版與上層研究版分開維護；研究版仍保存舊命令列和自製評分器，不能將研究目錄整包提交。外部測試入口是倉庫的 `tools/benchmark_submissions.py`，完整結果與平台驗證狀態見倉庫的 `SUBMISSION_REVIEW_20261030.md`。
