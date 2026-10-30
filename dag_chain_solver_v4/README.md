# DAG Chain Solver v4 (0.4.0)

獨立的 C++17 多核調度方案生成器。輸入計算圖 JSON，輸出題目 Scene B 要求的
`node_to_subgraph` 與 `core_schedules`。不依賴 Python、外部評分器或第三方 C++ 函式庫。

## 編譯與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 4 -o output/plan.json
```

Windows 執行檔為 `build/bin/npu_sched.exe`；Visual Studio 多組態建置也放在相同的 `bin`。
可用 `--cores=4`、`--output=output/plan.json`。省略核心數時使用 4 核；省略輸出路徑時，
寫入輸入檔同目錄的 `<stem>_multicore_res.json`。支援 2～5 核及 Unicode／空白路徑。
輸出目錄不存在時會建立，成功時不在 stdout 輸出評分。`--version` 顯示版本。

返回碼：0 成功、1 未預期錯誤、2 命令列錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用方案。
生成器拒絕以輸出覆蓋輸入，失敗時保留既有輸出檔。

## 固定規則

以 2026-10-30 取得的 `dag_chain_solver_v2/hackathon.zip` 內 **config.txt 為準**，
不沿用尚未更新的文件數值：

| 項目 | 值 |
|---|---:|
| 每核 L1 | 1,572,864 bytes |
| 每核 UB | 131,072 bytes |
| 共享 DDR 帶寬 | 60 bytes/cycle |
| 跨核同步 | COPY_OUT 完成後等待 1,000 cycles，才可啟動 COPY_IN |
| 換出軟門檻 | 容量的 90%，向下取整 |
| Pipe | MTE2、MTE3、M、V、FIX 各自單發射 |

同一 Tensor 的所有計算寫入者必須在同核。跨不同目的核按各自的 COPY_OUT／COPY_IN 對計量。
允許部分寫入 Tensor 換出及重載；後續再次寫入會令舊 DDR 備份失效。
正式執行檔不讀取 config，不接受額外調參旗標。

## 演算法

1. 驗證原 DAG、收縮原始 COPY 所連接的依賴，將同一 Tensor 的寫入者綁定。
2. 共用最長路徑分析，完成時間取所有前驅最大值。以計算關鍵性和通訊量形成路徑，
   嘗試替代前驅，並按每條 Pipe 工作量及 Tensor 足跡切成段。
3. 多組權重將段分配到核心，產生不同拓撲順序。維持按估計時間、搬運量、
   計算排程時間及舊成本模型篩出的多樣候選；保留舊搜尋路徑的候選。
4. 嘗試整段搬移、寫入者群組搬移、Tensor 使用者合併、兩段交換及記憶體導向重排。
5. 對入圍方案建立 COPY／換出／重載事件，模擬固定 Pipe、跨核等待、記憶體容量重用
   依賴和共享 DDR。按細化估計 cycles 選出方案，搬運量用作同時間的次要準則。
6. 比較 2 到 N 核的獨立搜尋結果，使用相同選擇準則；允許空核心。

搜尋工作量由輸入規模和固定參數決定，不使用隨機數或牆鐘停止條件。最多同時使用 4 個
演算法執行緒（包含主執行緒），並以固定順序合併結果。

細化估價仍是自寫的啟發式：COPY 在子圖內的排列與記憶體重用依賴可能和官方模擬不同。
估價不代表正式得分或可行性證明；實際數值須由題目附帶的原評分器驗證。
增加可用核心數保證保留較小核心數的搜尋結果，不保證原評分器實測 cycles 單調下降。

## 開發與測試

完整工作目錄另外包含 `tests/`、`scripts/`、`reports/`；提交壓縮包只包含本文件、
`CMakeLists.txt`、`src/`。測試工具與原評分器不會進入正式執行檔。

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python scripts/prepare_reference.py
python tests/check_rules_outputs.py --solver build/bin/npu_sched --data references/hackathon_20261030/data --out runs/final/output_checks_msvc
python scripts/run_official_problem2.py
python scripts/summarize_results.py
python scripts/package.py --team YOUR_TEAM_ID
```

Windows 將上述 solver 路徑加上 `.exe`。Python 研究工具需要 Python 3.10+，僅用標準庫。
`prepare_reference.py` 只提取雜湊已核對的題目檔案，不提取壓縮包的 `.git`。
執行結果保存在 `runs/`，完整比較表保存在 `reports/`。修改生成器後請使用新的結果目錄，
避免將不同執行檔的方案與分數混合。提交包可獨立編譯；測試工具需完整工作目錄與題目附件。
