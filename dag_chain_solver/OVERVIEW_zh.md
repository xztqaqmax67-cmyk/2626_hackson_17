# DAG 鏈剖分求解器導讀

這是一個 C++17 多核排程研究原型：讀入算子 DAG，為 2～5 個核心生成排程 JSON，目標是縮短包含資料搬運與同步等待的執行完成時間（Makespan）。核心想法是先把關鍵依賴連成鏈，再切段、分核及調整執行順序。

## 算法流程

1. **分析 DAG**：建立拓撲序，估算每個算子的累積完成時間及下游關鍵路徑。
2. **建立鏈**：優先連接累積完成時間最晚的前驅，另嘗試帶通訊偏好的候選。分叉時每個節點只保留一條鏈內後繼，形成不重疊的路徑；其餘原始依賴仍保留。
3. **切段與分核**：按各 Pipe 的工作量切開長鏈，兼顧負載、跨核搬運及同步成本分配核心。同一 Tensor 的多個寫入者保守地安排在同核。
4. **核內排序**：從已就緒算子中選擇下一個操作，考慮關鍵路徑與記憶體生命週期。同核不同鏈可以交錯；最終每個算子獨立輸出為一個子圖。
5. **選擇候選**：比較多種切鏈尺度、逐算子貪心及單核候選，再做有限輪鏈段遷移。使用固定候選順序與迭代次數，不按時鐘或亂數選解。

內部估價只近似 Pipe 排程、DDR 流量和記憶體溢出；完整合法性與 Makespan 由開發階段的外部評估器檢查。內部估價較低，不保證外部評分一定更好。

## 編譯與執行

需要 CMake 3.16+ 和支援 C++17 的編譯器；求解器無第三方庫依賴。以下命令在 `dag_chain_solver/` 目錄執行，將 `graph.json` 換成實際輸入檔案。

Windows／Visual Studio：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe graph.json -n 4 -o plan.json
```

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched graph.json -n 4 -o plan.json
```

預設 4 核；輸出只有 `node_to_subgraph` 和 `core_schedules`。Windows MSVC 與 MinGW 已實測，Linux 尚待驗證。

按提交要求 v3.0（2026-10-30）固定 L1=1,572,864 bytes、UB=131,072 bytes、DDR=60 bytes/cycle、跨核等待=1,000 cycles，保留獨立的 `PIPE_FIX`。求解器不讀配置檔、不呼叫外部程序；搬運與同步計入模擬執行時間，不會讓求解器真的等待相應時長。

## 本地測試結果

2026-10-30，使用 `case_00`～`case_09`，各測 2、3、4、5 核。40 組鏈版方案均通過既有 C++ Scene B 評估器，重跑輸出一致。

以下 baseline 是同一輸入由既有 C++ Scene A 單核評估器計算的單子圖執行時間 `T1`。每例加速比為 `T1 / T_chain`，再對 10 個用例取算術平均；這不是主辦方正式分數。

| 核心數 | 相對單核 baseline 的平均加速比 |
|---|---:|
| 2 | 1.4945 倍 |
| 3 | 1.9872 倍 |
| 4 | 2.3616 倍 |
| 5 | 2.0974 倍 |
| 全部 40 組 | 1.9852 倍 |

40 次求解程式耗時合計約 32.83 秒，單次最長約 2.11 秒，包含程式啟動與檔案讀寫，不含外部評估。這是本機測量，與上表的模擬執行加速比是不同指標。

另一個對照程式 `chain_baseline` 是「同一估價模型，移除鏈候選與鏈段遷移」的消融基線，與上表單核 baseline 不同。鏈版相對它為 20 組更快、18 組相同、2 組更慢；詳見 [EXPERIMENTS.md](EXPERIMENTS.md)。

## 接手開發

| 檔案 | 內容 |
|---|---|
| `src/chain.cpp` | 鏈覆蓋、切段、分核、遷移及候選選擇 |
| `src/model.hpp` | 圖讀取、依賴分析、核內排序、搬運及溢出估價；改編自本庫 `hackathon_v3/solver.cpp` |
| `src/main.cpp` | CLI、路徑處理及標準方案寫出 |
| `tests/` | 算法不變量、CLI 契約及外部評估驗證 |

優先改善內部估價與外部 Makespan 的排序偏差，尤其是 DDR 競爭與溢出預測。`case_05／3 核`、`case_06／4 核` 目前比消融基線慢；增加核心也不保證更快。多寫入 Tensor 同核是算法的保守策略，不是新增賽規。

修改後使用 [README.md](README.md) 的開發測試命令，對 10 個用例的全部四種核心數重新做外部驗證並記錄搬運量。測試腳本需另提供輸入資料及 `parity_runner`，求解器本身可獨立構建和執行。

GitHub 可保留原始碼、測試、腳本及說明文件；本目錄 `.gitignore` 已排除 `build*/`、`runs/` 和 `dist/`。詳細原始結果位於本機 `runs/all_cores/report.json`，單核基準來源為同庫 `前辈代码_cpp/output/review_20261030_msvc/final_summary.json`，這些報告不會隨預設 Git 提交上傳。比賽提交包則由 `scripts/package.py` 另外產生，僅包含 CMake、README 與 `src/`。
