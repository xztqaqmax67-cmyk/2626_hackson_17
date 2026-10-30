# DAG Chain Solver v6（0.6.0，30 秒搜尋版）

按使用者要求，將 `dag_chain_solver_v5_30s`（0.5.1）另存並命名為 v6。
本次只更新版本識別、文件與相關工具中的版本名稱；演算法、搜尋上限和題目規則完全沿用 30 秒版。
`dag_chain_solver_v5`（0.5.0）維持「每點約 30 秒」要求之前的固定工作量版本，
已與當時保存的 59 個檔案雜湊逐一核對。版本關係見 [CHANGES_v6_zh.md](CHANGES_v6_zh.md)。
先完整取得原 v5 的最佳種子方案，再利用剩餘時間追加搜尋，目標是每個輸入約 30 秒。
這是 C++17 Scene B 方案生成器，輸出 `node_to_subgraph`、`core_schedules`，
正式執行不呼叫 Python、官方評分器或其他外部程序，也不讀取已知測試答案。

## 編譯與執行

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
build/bin/npu_sched input.json -n 5 -o output/plan.json
```

Windows 加上 `.exe`，例如 `build/bin/npu_sched.exe`。支援 2～5 核及 Unicode／空白路徑，
省略核心數時使用 4 核；本次效能測試只使用 5 核。省略輸出路徑時寫到輸入檔同目錄的
`<stem>_multicore_res.json`。`--version` 顯示 0.6.0。

返回碼：0 成功、1 非預期錯誤、2 CLI 錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用方案。
不允許覆蓋輸入；寫出方案仍採原有的暫存檔與完成後替換流程。
正式 CLI 沒有新增 `--time-limit` 或調參選項。要修改時間和搜尋寬度，請閱讀
[搜尋上限說明](SEARCH_LIMITS_zh.md)，修改 [src/search_limits.hpp](src/search_limits.hpp) 後重新編譯。

## 約 30 秒的含義

計時起點在讀取／解析輸入前。所有搜尋執行緒共用同一個截止時間，並不是每個執行緒各跑 30 秒。
目前 `total_seconds=30.0`，預留 `timed_reserve_seconds=0.15` 秒給收尾與輸出。
測試紀錄的程序牆鐘時間另包含程序啟動、資源監測和輸出，實際值不必剛好為 30.000 秒。

原 v5 種子先完整完成，以保留可輸出的方案。如果種子階段已耗盡預算，便不再追加搜尋。
因此超大輸入不保證能在 30 秒內完成；取消是程式內定期檢查截止時間的合作式取消，
也不是作業系統強制中止。單次尚未檢查截止時間的工作、執行緒收尾、驗證及 I/O 可能造成少量超時。

若保留候選池的鄰域已探索完，或去重簽章達到上限，會提前結束。
程式沒有睡眠、空轉或重複評分相同候選來填滿時間。

這個「30 秒」是本機生成方案的時間；報表的 **cycles** 是官方模擬 NPU 排程的完成時間。
兩者不是同一個量。1000 cycles 的跨核等待也不是生成器真的睡眠等待。

## 追加搜尋

原 v5 的鏈剖分、粗階段自然連通塊、核心配額與等待餘量候選全部保留。
種子完成後，最多 4 個獨立搜尋工作（包含呼叫執行緒）在完整可用核心集合上探索，
各自持有候選池、去重紀錄和最佳已完成方案；原圖唯讀，沒有巢狀開啟搜尋執行緒。

追加候選有八類：

1. 固定核心分配，變換排序方式、ready 視窗與記憶體權重。
2. 搬移完整 writer 群組。
3. 交換兩個 writer 群組所在核心。
4. 將一個 Tensor 的使用者及其 writer 閉包合併到同核。
5. 搬移完整鏈段。
6. 擴大未固定核心分配時的負載／通信權重組合。
7. 旋轉 ready 節點的選取優先序，仍按原 DAG 產生合法拓撲順序。
8. 聯合搬移兩個 writer 群組。

每輪先做全圖粗估，再對少量候選進行共享 DDR 事件細化。只有**完整完成且通過內部檢查**的候選
才可替換最佳方案；截止時間在候選中途到達時，該候選被丟棄，保留之前的完整方案。
最後等待工作結束並以固定準則合併結果。

## 可重現性與規則

候選枚舉沒有使用亂數，但牆鐘截止會受到 CPU 頻率、背景負載、快取和執行緒排程影響，
所以每次完成的候選數可能不同，輸出的方案與官方 cycles 也可能不同。
**不要再要求計時版重跑後方案位元組完全相同。** 每次實測應保存實際產生的方案及其雜湊，
再評分這一份方案。原 v5 的固定工作量搜尋仍可透過開發 API 單獨研究。

沒有放鬆題目規則：

- 原始資料／控制依賴保持完整；拆鏈只解除同核偏好，不刪除依賴，也不新增整層等待屏障。
- 每個運算恰好分配一次；同一 Tensor 的所有寫入者保持同核，跨階段亦同。
- L1 1,572,864 bytes、UB 131,072 bytes、共享 DDR 60 bytes/cycle 保持不變。
- 源 COPY_OUT 完成後等 1000 cycles，目標 COPY_IN 才可開始；換出軟門檻為硬容量的 90%。
- 全圖仍檢查拓撲／writer 約束、記憶體估算、Pipe FIFO、跨核 COPY 與記憶體重用事件。

內部細化沿用自寫估價，仍可能與官方 COPY 排序及容量重用細節不同。
保留種子只保證內部選優準則不因追加搜尋而變差，不保證官方結果一定更好。
有限測試與自寫估價也不是任意未知輸入可行性的形式證明。
所有策略依輸入結構運作，不按公開用例編號、檔名或已知答案設例外。

## 測試方式

完整開發目錄包含 `tests/`、`scripts/`、`reports/` 和本地 `references/`。
提交源碼不依賴這些測試資料。以 `CHAIN_BUILD_TESTS=ON` 建置時會額外生成 `npu_sched_quick`，
它使用 0.50 秒測試預算，供 CLI、輸入契約與一般 DAG 合法性測試，不作正式 30 秒成績。

```sh
cmake -S . -B build -DCHAIN_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python scripts/prepare_reference.py
python tests/contracts.py --solver build/bin/npu_sched_quick --out runs/quick_cli
python tests/rules_contracts.py --solver build/bin/npu_sched_quick --out runs/quick_rules
python tests/run_v5_synthetic.py --solver build/bin/npu_sched_quick --out runs/quick_synthetic
```

Windows 將上面的 quick 執行檔名稱加上 `.exe`。以下是改名之前 0.5.1 的歷史驗證：
MSVC 與 MinGW 各 7 組 C++ 測試通過，MSVC quick 版另通過 CLI 契約、26/26 組圖輸入契約，
以及 64/64 組合成 DAG 的官方評分驗證。本次另存 v6 沒有重跑官方評分。
本機編譯器為 MSVC 19.44、GCC/MinGW 15.1；尚未測試原生 Linux 或題目指定的較舊 GCC。
舊版 `check_rules_outputs.py` 等工具含重跑位元組一致性要求，不適用於計時版的正式測試。

公開 10 例只測 5 核，先**逐例串行生成全部方案**，再執行原評分器，避免評分工作干擾生成計時：

```sh
python scripts/run_five_30s.py --solver build/bin/npu_sched --out runs/five_30s
python scripts/summarize_five_30s.py
```

亦可分成 `--stage generate`、`--stage evaluate` 兩階段；評分階段使用同一 `--out`。
每次更換源碼／執行檔後都應使用新的結果目錄，不要把前後方案混在一起。
改名前的生成時間、正式 cycles、相對單核加速比、搬運量和原始檔案位置見
[0.5.1 五核歷史結果](../dag_chain_solver_v5_30s/reports/FIVE_CORES_30S_zh.md) 與
[歷史 JSON](../dag_chain_solver_v5_30s/reports/FIVE_CORES_30S.json)。
歷史記錄保留原執行檔／源碼雜湊，沒有改標成 v6 的新測試。
2026-10-30 的該次 10 例均通過官方評分，平均生成時間 29.957 秒。
相對官方單核基準的平均加速比由原 v5 的 2.195383× 提升至 2.254789×；
8 例 cycles 降低、2 例持平，總搬運量下降 2.8314%，但有 2 例搬運量增加。
這是先前保存方案的單次實測，不是重跑必定相同的保證。
v6 後續獨立執行上方測試流程時，才會建立本資料夾的 `runs/` 與正式結果報表。

`python scripts/package.py` 產生源碼提交包，只包含 `CMakeLists.txt`、本說明、
`SEARCH_LIMITS_zh.md`、`CHANGES_v6_zh.md` 和 `src/`；解壓後按預設方式編譯即可，不開啟開發測試選項。

## 接手位置

- [src/search_limits.hpp](src/search_limits.hpp)：時間和搜尋上限的集中設定及中文註解。
- [src/anytime.cpp](src/anytime.cpp)：追加搜尋、候選池、去重、截止時間與最佳方案保留。
- [src/model.hpp](src/model.hpp)、[src/refine.cpp](src/refine.cpp)：合作式取消與原有估價。
- `reports/v5_preservation_manifest.json`：原 v5 檔案雜湊。
- `reports/v5_results_baseline.json`：原 v5 的既有結果，僅供研究比較，生成器不讀取。
