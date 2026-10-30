# DAG Chain Solver v6（0.6.1，約 15 秒搜尋版）

本版在原 v6 的演算法上加入實作效率優化，並把總搜尋預算從 30 秒改為 15 秒。
選優準則、候選枚舉、DAG 依賴、writer 同核限制、容量、共享 DDR 與同步規則保持不變。
修改前完整源碼、工具及執行檔保存在 `snapshots/v6_0_6_0_before_perf_15s/`，包含 SHA256 清單。
`../dag_chain_solver_v5/` 仍是時間限制調整前的 0.5.0；未修改。

## 建置與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 加上 `.exe`。正式介面支援 2～5 核，預設 4 核；五核是可用上限，不要求全部使用。
`--version` 顯示 `dag-chain-solver 0.6.1`。輸出仍只有 `node_to_subgraph`、`core_schedules`。
不接受額外調參旗標，不讀 config／已知答案，不執行 Python 或外部評分器。
返回碼為 0 成功、1 非預期錯誤、2 CLI 錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用方案。
失敗時保留既有輸出，拒絕覆蓋輸入。

## 本次效率優化

- 比較鍵只保存數值與方案向量的唯讀引用，避免每次排序／比較複製完整 core、order。
- writer 群組及鏈段的工作量在排序前計算一次，保持原求和順序和穩定排序規則。
- Graph 建構時預先保存每個 Op 的 Tensor 使用聯集及每個 Tensor 的使用者聯集；
  依原用途保留原順序或排序順序，供 MemoryOrder 和相關估價流程重用。
- 不需要核心分配副本的候選分支，不提前複製該向量。

快取在讀圖時建立，之後隨唯讀 Graph 分享，不使用跨執行緒延遲初始化。
沒有新增大型評估結果快取，也沒有更換官方換出策略。

## 約 15 秒的含義

[src/search_limits.hpp](src/search_limits.hpp) 的 `total_seconds=15.0` 是共同牆鐘預算，
從讀圖前起算；先完整執行原 v5 種子搜尋，再用剩餘時間追加搜尋。
最多 4 個搜尋工作（含呼叫執行緒），共用起點後約 14.85 秒的截止點，另預留 0.15 秒收尾。
如果初始種子或解析已超過預算，便不再追加搜尋；這不是任意大小輸入的硬即時保證。
若候選空間耗盡可提前結束，沒有睡眠或空轉填滿時間。

取消只丟棄未完成候選，保留已完成的方案。時間截斷會使不同次運行完成的搜尋量不同，
所以不要求計時版的輸出逐位元相同。固定工作量的效率比較另行測試，不能把計時搜尋
產生不同方案誤認為語義不等價。

生成秒數是這臺電腦運行求解器的時間；官方 cycles 是模擬 NPU 執行時間，兩者不同。
更完整的參數解釋見 [搜尋上限說明](SEARCH_LIMITS_zh.md)。

## 驗證與測試工具

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python tests/contracts.py --solver build/bin/npu_sched_quick --out runs/quick_cli
python tests/rules_contracts.py --solver build/bin/npu_sched_quick --out runs/quick_rules
python tests/run_v5_synthetic.py --solver build/bin/npu_sched_quick --out runs/quick_synthetic
python scripts/run_five_15s.py --solver build/bin/npu_sched --out runs/five_15s
python scripts/summarize_five_15s.py --solver build/bin/npu_sched
```

`npu_sched_quick` 是 0.50 秒開發測試版本；正式執行檔使用 15 秒。
公開測試先串行生成全部方案，再評分，避免評分干擾生成耗時。
舊的 `run_five_30s.py` 已停止使用，防止把 15 秒程式的結果錯標成 30 秒。

固定工作量效率比較見 `scripts/check_perf_15s.py`，以相同編譯器的修改前後
`chain_diagnostics` 比較完整初始搜尋，要求方案和非時間診斷值一致。
主要耗時計入程序啟動、讀圖、建立快取、求解與寫出；不把前處理搬出計時區間。
這一測量不代表追加搜尋所有分支的加速倍數。

本次 MSVC 19.44 與 GCC/MinGW 15.1 各通過 9 組 C++ 測試；MSVC quick 版通過
CLI 契約、26/26 圖輸入契約與 64/64 合成圖官方評分。
固定初始搜尋以 GCC/MinGW 對 3 個用例各交替量測 3 次，全部方案與非時間診斷一致；
程序耗時中位數合計由 17.981 秒降至 15.179 秒，降低 15.583%（1.1846 倍）。
詳見 [固定工作量效率報告](reports/PERF_15S_zh.md)。這不等於 NPU cycles 改善比例。
尚未測試原生 Linux 或題目指定的較舊 GCC。

本次五核 10/10 用例通過官方評分；生成時間為 14.892～14.989 秒，平均 14.939 秒。
平均單核加速比為 2.250554×，歷史 30 秒版為 2.254789×（本次低 0.188%）。
與歷史 30 秒版相比，1 例 cycles 改善、4 例持平、5 例增加；總 cycles 增加 0.423%，
總搬運增加 0.317%。這是同時改變效率及預算後的單次實測，並非純效率比較。
逐點數值與搬運分類見 [15 秒結果表](reports/FIVE_CORES_15S_zh.md) 和
[完整 JSON](reports/FIVE_CORES_15S.json)。原始方案及官方 trace 位於 `runs/five_15s/`。

原 30 秒版的已保存官方結果在
[歷史結果表](../dag_chain_solver_v5_30s/reports/FIVE_CORES_30S_zh.md)。
它們保留原版本與執行檔雜湊，沒有改標成本版新測試。
內部估價並非官方評分器的形式化等價證明；公開與合成測試也不能證明所有未知輸入。

## 文件與打包

[版本記錄](CHANGES_v6_zh.md) 說明原始 v5、30 秒 v6 與本次版本的關係。
`python scripts/package.py` 產生 `dist/dag_chain_solver_v6.zip`，只包含 CMake、README、
搜尋上限／版本說明與 src；不含測試資料、快照、報表和執行檔。
解壓後以預設選項建置，不開啟開發測試選項。
