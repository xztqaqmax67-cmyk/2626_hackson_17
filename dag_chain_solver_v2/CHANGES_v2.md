# DAG 鏈剖分 v2 改動說明

版本 `0.2.0`，日期 2026-10-30。這一版針對多寫入 Tensor 造成候選被排除，以及較多核心沒有沿用較少核心好方案的問題進行修改。

原版保留在同庫的 `dag_chain_solver/`，新版在 `dag_chain_solver_v2/`，兩者可獨立編譯。`reports/v1_source_manifest.json` 記錄修改前原版原始碼、測試及說明文件的 SHA-256，供核對原版未被改寫。

## 多寫入 Tensor 的排序修復

原版偵測到 Tensor 尚有後續寫入、卻需要被換出時，直接淘汰候選。診斷曾發現 case_08 五核的 20 個候選中，19 個被此檢查排除；抽查兩個被排除方案，外部評估器均確認死鎖。

v2 保留這項檢查，新增以下修復流程：

1. 溢出估價記錄發生風險的 Tensor。
2. 將該 Tensor 所有寫入者的排序優先級提前，並向前傳遞至它們需要的前置操作。
3. 在拓撲約束下重新排序；同一優先級先準備前置操作，再開始寫入，減少部分寫入期間插入無關工作。
4. 最多嘗試兩輪修復，再試一個純依賴 DFS 順序。每個候選重新做完整的內部估價；仍有容量或部分寫入風險者繼續排除。

修復只調整操作順序，保留核心分配與全部 DAG 依賴。同一 Tensor 的多個寫入者仍安排在同核。

## 較少核心數的候選

要求 N 核時，依次生成 2 到 N 核的候選，每個核心數只搜尋一次；每組另保留單核候選。選中的方案可以少用核心，輸出仍有 N 個核心列表，未使用者為空。

跨核心數比較採較保守的替換條件。令 `B` 為估計總搬運量、`S` 為估計溢出搬運量、`Tsketch` 為未完整模擬共享頻寬的時間草圖：

- 較少核心方案的偏保守時間估計：`max(Tsketch + S / 60, B / 60)`。
- 目前方案的樂觀時間估計：`max(Tsketch, B / 60)`。

先以最大可用核心數的方案為基準。較少核心的候選須同時有較低的原始總估價，且偏保守時間估計不高於目前方案的樂觀估計，才替換。這項條件用來抑制「只因搬運量下降，就過度犧牲計算並行性」的選擇。

兩種估計都不是外部 Makespan 的嚴格界限，因此仍可能誤選，也不保證增加核心後的實際結果單調改善。

## 程式變更位置

| 檔案 | 變更 |
|---|---|
| `src/model.hpp` | 記錄部分寫入時被換出的 Tensor；保留既有溢出及容量檢查 |
| `src/chain.cpp` | 寫入者與前置操作排序修復、修復候選重評、跨有效核心數搜尋與保守選擇 |
| `src/chain.hpp` | 修復介面及候選診斷資訊 |
| `src/main.cpp`、`CMakeLists.txt` | 版本更新為 0.2.0，輸入輸出介面不變 |
| `tests/chain_tests.cpp` | 補充部分寫入、前置操作、跨核心選解的回歸測試 |
| `tests/compare_v1.py` | 原版重現、新版重跑與外部評估對照 |

## 測試與結果

最終比較見 [EXPERIMENTS.md](EXPERIMENTS.md)，逐例數據見 `reports/comparison_20261030.json`。

比較時先重新執行原版，要求輸出與原先已評估方案逐位元組相同，且輸入 SHA-256 相同，才沿用原版 Makespan 與搬運量。新版每組生成兩次比對輸出，並交由同一外部 C++ Scene B 評估器重新計算。核心數為 2、3、4、5，10 個用例合計 40 組。

平均加速比統一使用同一份本地單核 Scene A 基準 `T1 / TN`，不是主辦方正式分數。求解耗時採同輪原版／新版依序生成的測量，與模擬完成時間分開記錄。

編譯和一般執行方式見 [README.md](README.md)。在專案根目錄重現版本比較：

```powershell
python dag_chain_solver_v2/tests/compare_v1.py --v1 dag_chain_solver/build/bin/npu_sched.exe --v1-runs dag_chain_solver/runs/all_cores --v2 dag_chain_solver_v2/build/bin/npu_sched.exe --evaluator 前辈代码_cpp/build/review_20261030_msvc/bin/Release/parity_runner.exe --data hackathon_data --singlecore-report 前辈代码_cpp/output/review_20261030_msvc/final_summary.json --out dag_chain_solver_v2/runs/comparison_repeated
```

## 仍需處理的問題

內部估價仍近似共享 DDR、FIFO 等待與非同步記憶體重用。候選修復通過內部檢查不等同於通過外部評估，擴大候選集也不保證每例都改善。這版沒有針對檔名、用例 ID 或預生成答案作特判，正式求解器不載入測試報告、不呼叫評估器，也不輸出分數。

搜尋使用固定次數，保持確定性；更多候選會增加求解耗時。公開資料的耗時、合法性和跨編譯器檢查以實驗紀錄為準，Linux 仍需在目標環境驗證。
