# DAG Chain Solver v5 (0.5.0)

從 v4 獨立建立的 C++17 Scene B 排程生成器。v4 與原評分器保留不變。
v5 增加「粗階段的自然連通塊核心配額」與「消費者等待餘量拆鏈」候選，
保留完整 v4 搜尋及其細化估價最佳方案後，再比較新候選。

## 建置與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 4 -o output/plan.json
```

Windows 執行檔為 `build/bin/npu_sched.exe`。支援 2～5 核、Unicode／空白路徑；
省略核心數使用 4，省略輸出路徑使用 `<輸入檔名>_multicore_res.json`。
輸出只有題目要求的 `node_to_subgraph`、`core_schedules`；允許空核心。
1 核只作內部區域配額，正式 CLI 不新增 `-n 1`；報表中的 1 核為原評分器單核基準。

返回碼：0 成功、1 非預期錯誤、2 CLI 錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用方案。
拒絕覆蓋輸入；失敗時保留既有輸出。`--version` 輸出 0.5.0。
正式生成器不呼叫外部程序、不讀取 config／報表／測試答案，不需要 Python 或第三方 C++ 庫。

## 規則與合法性邊界

參數沿用題目附件的 config：L1 1,572,864 bytes、UB 131,072 bytes、
共享 DDR 60 bytes/cycle；源 COPY_OUT 完成後等 1,000 cycles，目標 COPY_IN 才能開始。
換出軟門檻是硬容量的 90%，當前必要輸入／輸出受保護；各 Pipe 單發射。

- 原始 Graph 始終唯讀，沒有刪除或放鬆任何資料／控制依賴；沒有新增整層同步屏障。
- 每個運算恰好分配一次，所有同 Tensor 寫入者保持同核；跨階段 writer 約束同樣有效。
- 區域分組只影響核心選擇；完整原 DAG 仍由全域 ready 排程產生拓撲順序。
- 拆鏈只解除同核偏好；COPY、同步、DDR 競爭和 spill 仍進入完整圖估價。
- 全圖驗證包含依賴、writer 和容量估計，細化事件包含 Pipe FIFO、COPY 與記憶體重用依賴。

細化估價沿用 v4 的自寫模型，不等同官方 Step1/2/3 的全部行為，**不是任意輸入的正式可行性證明**。
一般圖測試用來發現違例，有限測試不能證明所有未知輸入都合法，也不能保證所有輸入都找到可行解。
新策略不以公開用例編號、檔名、已知答案或圖模板選參數；輸入規模只用來限制搜尋工作量。

## 新增流程

1. 完整執行 v4 原搜尋與共享 DDR 細化估價，保留其最佳候選。
2. 按單調加權拓撲深度建立粗階段；在階段內找忽略方向的連通塊，納入 writer 綁定。
   常規深度區間與自然分叉／匯合分界提供少量候選；分區不改變原圖。
3. 對有 2～k 個自然塊的階段枚舉正整數核心配額及互不重疊的實體核心集合，總數不超過 k。
   包含 1+4、2+3 等情況，沒有每塊最多 3 核的硬限制。單塊或塊數超過核心數的階段沿用 v4 分配。
4. 以區域 Pipe 負載、區域關鍵路徑與邊界資料歸屬粗篩配額；在各核心集合內沿用鏈分段和貪心放置。
   跨階段 writer 群組固定在種子方案的核心，避免局部分配破壞全域約束。
5. 另重播種子方案，依每個消費者自身的開始時間篩選可換核的 writer 群組。
   新資料估計到達不得超過受影響消費者的原開始時間；再全圖重播少量候選。
   這是候選篩選，不把全圖最慢核心或 1,000 cycles 當成免費等待。
6. 新候選回到完整 DAG 排序、記憶體估算與事件細化；與已保留的 v4 方案使用同一準則比較。

核心配額枚舉是局部粗篩，不枚舉所有階段的全域笛卡兒積，不宣稱全域最優。
新搬運可能影響其他分支，即使局部等待餘量檢查通過也不保證整圖更快。
保留 v4 方案可保內部細化準則不退步，不能保證官方實測 cycles 不退步。
候選數和工作上限固定且由輸入規模決定；沒有隨機數或牆鐘截止。
最多 4 個搜尋執行緒（含主執行緒），沒有新增巢狀平行化。

## 測試與結果

本機 MSVC 19.44 與 GCC/MinGW 15.1 各通過 6 組 C++ 測試、CLI 契約與 26 項輸入契約。
16 個獨立人工／固定種子隨機輸入 × 2～5 核，原評分器 64/64 通過；公開用例 40/40 通過，
每組重複生成兩次完全一致。另核對 4 組 MSVC/GCC 輸出一致及 9 組停用新策略後重現 v4。

公開 40 組平均單核加速比：v4 1.909483× → v5 1.914700×（+0.2732%）；
cycles 合計下降 0.5052%，總 COPY 下降 1.3026%。5 組 cycles 改善、35 組相同、0 組退步。
三組改善來自 slack 候選、兩組來自區域候選。每組搬運量不保證下降，逐例完整保留於報表。
最大觀測生成耗時 8.27 秒、working set 174.35 MiB、程序執行緒 7（包含執行期執行緒）。
這些是本機觀測，不是未知輸入上限保證，也未以它們推論相對 v4 的 CPU 加速比。
尚未測試原生 Linux、指定較舊版本 GCC 或 ThreadSanitizer。

完整工作目錄包含 tests、scripts、reports 和本地 references；提交壓縮包只有 CMake、README、src。

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python scripts/prepare_reference.py
python tests/run_v5_synthetic.py --solver build/bin/npu_sched --out runs/synthetic_final
python tests/check_rules_outputs.py --solver build/bin/npu_sched --data references/hackathon_20261030/data --out runs/final/output_checks_msvc
python scripts/run_official_problem2.py
python scripts/summarize_results.py
python scripts/package.py
```

Windows 將 solver 名稱加上 `.exe`。更換程式後需使用新結果目錄，避免混用來源。
結果見 [v4/v5 比較](reports/RESULTS_1_TO_5_CORES_zh.md)；精確數值見
[50 筆 JSON](reports/RESULTS_1_TO_5_CORES.json)。提交包不包含這些研究產物。

開發工具 `chain_diagnostics graph.json 5 output.json --extensions 0` 可停用新候選以核對 v4 路徑；
1 為僅區域、2 為僅 slack、3 為兩者。這些開關只在測試 API／診斷程式，不是正式 CLI。

原始碼位置：`src/regions.cpp` 是階段與配額；`src/slack.cpp` 是等待餘量候選；
`src/chain.cpp` 整合搜尋；`src/refine.cpp` 與 `src/model.hpp` 沿用 v4 的估價和圖模型。
