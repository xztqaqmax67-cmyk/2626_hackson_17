# 給新電腦與 GPT 的交接摘要

更新：2026-10-30。目標是用 C++17 為 Hackathon 的計算圖產生分核與調度 JSON，並比較本地與舊數模評分器。

**2026-10-30 提交規範更新：**先讀 [SUBMISSION_REVIEW_20261030.md](SUBMISSION_REVIEW_20261030.md)。可提交工程分別位於 `hackathon_v3/submission/`（舊算法）與 `前辈代码_cpp/submission/`（模組化新算法）。兩者輸出 `npu_sched`、固定硬體參數、支援 2～5 核，無評分器與子程序。以下上層 `solver.cpp`／`score.py` 等為保留的研究版，命令列與同步模型可能不同，勿整包提交或用研究版成績代表提交版。

## 先找這些檔案

| 位置 | 用途 |
|---|---|
| `hackathon_v3/solver.cpp` | 目前求解器；依原 problem2 方法更新多初解、重排序與局部搜尋 |
| `hackathon_v3/score.py` | 呼叫 solver／模擬器，統計單核與多核時間、求解耗時、DDR 搬運量 |
| `hackathon_v3/local_eval/` | 自行重建的 v3 事件模擬器，**不是官方實作** |
| `hackathon_v3/config_LOCAL_ASSUMPTIONS.txt` | 最新硬體參數；有衝突時以新版設定為準 |
| `hackathon_data/` | 10 個新用例，與開發時資料的 JSON 內容相同 |
| `前辈代码/problem2/official_code/` | 原數模比賽評分器，保留原樣 |
| `hackathon_v3/compare_original_problem2.py` | 用原評分器評估已存四核方案，產生獨立對照結果 |
| `hackathon_v3/report_v3_all/` | 已完成的 v3 十例報告、方案與模擬摘要 |
| `hackathon_v3/report_original_problem2/` | 原評分器對照表、配置副本及每例狀態紀錄 |

## 使用者已確認的配置

- L1=1.5M，按 **1,572,864 bytes** 解讀；UB=128K，按 **131,072 bytes** 解讀。
- DDR 共用總帶寬 **60 bytes/cycle**；跨核同步延遲 **1000 cycles**。
- 新提交規範指定 **2、3、4、5 核**；單核基準由主辦方內部提供。本地單核數字只供對照。
- 舊 `problem2/config.txt` 的 L1=512 KiB、同步=500 不用作主要對照；不要修改原檔，使用獨立新版配置。

## 新電腦先執行

需要 Python 3.10+（建議 3.11）與支援 C++17 的 g++，Python 不需第三方套件。
進入 `hackathon_v3` 後，在 Windows PowerShell 執行：

```powershell
.\run_all_windows.ps1 -Cases case_01,case_07 -Out runs/smoke
```

若 Python 不在 PATH，加入 `-Python '完整路徑\python.exe'`。完整十例使用 `.\run_all_windows.ps1`。
Linux／WSL 使用 `bash run_all_linux.sh`。新輸出在 `runs/`，不覆蓋已提交的參考報告。
完整個別命令見 [hackathon_v3/README_zh.md](hackathon_v3/README_zh.md)。

## 已驗證的結果

- v3：**24 項單元測試通過**；10／10 用例通過本地模型約束檢查。
- 4 核、每例 1 秒求解軟預算：平均加速比 **2.1303412**；DDR 總量 **2,491,414,400 bytes**。
- 原評分器不接受原新資料的 `COPY cycles=-1`。只在副本改成 0 後，case_00～06 可完成單核與多核評估。
- case_07～09 使用 `PIPE_FIX`，原版不支援，仍被拒絕；不能宣稱十例全數通過原評分器。
- 同樣新版配置、同一批成功的 7 例：原評分器平均加速比 **2.3408667**，v3 **2.4798043**；DDR 搬運量逐例相同。
- 原評分器 case_05 的單核評估約需 239 秒；重跑對照建議 `--timeout 300` 或更長。

## 接手時必須知道的限制

1. v3 是重新實作版本，不是之前無法下載的 v2 ZIP 原檔。
2. v3 跨核規則為 `consumer.start >= producer.end + 1000`，且需完成 COPY_OUT→DDR→COPY_IN。
   原版則在 COPY_OUT 結束後等待 delay 才能 COPY_IN；相同數字不代表相同同步語義。
3. v3 支援五條 Pipe，DDR 動態平均分享帶寬；非 DDR 操作按原 cycles 執行。
4. 同一 Tensor 的多個寫入者固定同核。這是保守實作限制，不是題目已確認的額外限制。
5. 溢出採下一次使用最晚者，包含後續寫入；寫入會使舊 DDR 備份失效。
6. v3 的固定 Pipe 優先序與虛擬記憶體重用依賴可能限制重疊，並非官方核內啟發式的精確重現。
7. solver 的內部估價不是事件模擬成績。40／30／30 權重缺少百分制換算公式，目前不計算官方總分。
8. 原圖內部 COPY 橋接與跨核多寫入合併尚不支援；失敗時應保留錯誤，不能略過約束讓測試通過。

可繼續的工作：校準兩種評分器的排序／同步語義，改善較慢用例的分核策略，取得正式評分器與完整配置後重新驗證。
修改前先閱讀程式和現有報告；保留原評分器及測試輸入，以新輸出目錄記錄實驗。
Windows 的 solver.exe、Python 快取、大型時間線、暫存資料副本和失敗的早期 smoke 報告未納入 Git，可由腳本重建。
