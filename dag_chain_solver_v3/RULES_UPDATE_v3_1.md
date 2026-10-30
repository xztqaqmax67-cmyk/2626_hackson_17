# v0.3.1：依新版官方題目包調整估價與契約

本次對照使用者提供的 `dag_chain_solver_v2/hackathon.zip`；研究用副本位於 `references/hackathon_20261030/`。v0.3.1 修正求解器對溢出與多寫入 Tensor 的假設，保留 v3 的多執行緒搜尋。原 v2 目錄未替換，v0.3.0 已保存為 `snapshots/v0.3.0_before_rules_update.zip`。

官方評分器已收到。依使用者先前「先不要跑評分」的指示，本次未執行評分，也尚未完成官方完整執行流程的合法性或分數驗證。本文件說明程式修改，不宣稱分數、排程加速比或求解速度提升。

## 採用的規則與範圍

新版 `README.md`、`code/evaluation_validation.py`、`code/stub_multicore_cut_and_schedule.py`、`code/schedule_step2.py` 等是本次規則對照來源；C++ 程式的提交介面仍依研究根目錄的 `材料/提交要求文档.md`。

| 參數 | 本版使用值 | 溢出估價軟門檻 |
| --- | --- | --- |
| L1 硬容量 | 1572864 bytes | 1415577 bytes |
| UB 硬容量 | 131072 bytes | 117964 bytes |
| DDR 共享總帶寬 | 60 bytes/cycle | — |
| 跨核 COPY 等待 | 1000 cycles | — |

容量、帶寬與等待值採用新包 `data/config.txt` 及既有 C++ 提交要求。新包部分 `docs/` 說明殘留 L1 `524288` bytes、跨核等待 `500` cycles 的舊值，與上述來源不一致，本版不採用。軟門檻為 `floor(硬容量 × 90%)`，不改動硬體容量常量。

評分器的 `--config` 屬於評分工具介面；C++ 提交要求明定 solver 不接受配置檔參數、也不讀取 `config.txt`，因此仍使用固定常量及 `npu_sched <graph.json> -n <2..5> -o <plan.json>`。本次只對齊問題 2／場景 B，不新增題目 1 的 Task 等待模型或題目 3 的 Cache 模型。新包 10 個案例解析後的 JSON 語義與原有資料相同，檔案位元組不同不代表圖內容有新變化。

## 溢出估價修改

舊估價在配置單個 Tensor 時就以硬容量選擇換出資料。新版改為：

1. 每個操作先配置全部輸入、輸出，並將當前操作使用的 Tensor 排除於可換出集合。
2. 超過軟門檻時，按下次使用最晚的原則挑選合法 victim，估計寫回與後續重新讀入的搬運量。
3. 沒有合法 victim 時，只要駐留量不超過硬容量，容許該操作暫時超過軟門檻；若仍超過硬容量，才記為估價中的容量不可行。

這使估價能表達官方 Step2 的 90% 提前溢出策略及單次操作的軟門檻例外。它仍是按候選順序計算的近似模型，沒有完整模擬非同步搬運、每條 Pipe 的執行事件或真實記憶體生命週期。

## 多寫入 Tensor 的處理

官方 Step2 在換出部分寫入的 Tensor 時會同步重命名後續寫者；之後的寫入也會使原 DDR 備份過期。因此，「尚有寫者時曾換出」本身不能作為方案必定非法的判據。

v0.3.1 移除舊的 partial-write 直接拒絕條件，保留該現象作為排序搜尋訊號：嘗試提早完成相關寫者及其前置操作，以降低搬運量。原候選同時保留，由估價比較；調整後的候選也必須通過其餘檢查。既有 dirty／備份失效處理仍保留，Tensor 再被修改後，需要時必須重新寫回 DDR，不能沿用舊副本。

同一 Tensor 的全部計算寫者必須位於同一核心，現在已是新版官方方案合法性要求。除了候選生成時的寫者群組綁定，本版還在最終 `validate()` 中逐 Tensor 驗證寫者同核。

## 輸入與輸出契約

- 新增重複邊拒絕：以 `(source, target)` 判斷，附加 `data_size` 不同仍屬重複。
- 保留既有的五種必填 `op.pipe`，不從操作名稱推導 Pipe；任意計算操作名稱仍可使用。
- 保留 op／Tensor 共用非負唯一 ID 空間、有效端點、非負大小及週期檢查；`COPY_IN`／`COPY_OUT` 允許 `cycles=-1`。
- 保留重複 JSON 鍵拒絕，以及排除 COPY 前的原始完整依賴圖無環檢查。
- 輸出仍只有 `node_to_subgraph` 與 `core_schedules`，本次補上最終寫者同核驗證。

外層搜尋仍平行處理有效核心數 `2..N`：共 `N-1` 個計算執行緒，含主執行緒且最多 4 個；`N=2` 時單執行緒。共用唯讀 Graph，各工作寫固定槽位，全部 `join` 後按原次序合併。估價與候選保留條件已改變，v0.3.1 可能合理地產生不同於 v0.3.0 的排程；相同版本、相同輸入重複執行仍應完全一致。

## 非評分檢查與尚未涵蓋部分

在本研究目錄建置開發測試後，可執行：

```sh
ctest --test-dir build-dev -C Release --output-on-failure
python tests/contracts.py --solver build-dev/bin/npu_sched --out runs/contracts_v3_1
python tests/rules_contracts.py --solver build-dev/bin/npu_sched --out runs/rules_contracts_v3_1
python tests/check_rules_outputs.py --solver build-dev/bin/npu_sched --data references/hackathon_20261030/data --out runs/output_checks_v3_1
```

Windows 使用 `.exe` 路徑。最後一個腳本可加 `--previous-output-dir /path/to/previous_outputs`，記錄新版與既有方案的差異，不要求新舊版本逐位元組相同。這些檢查不執行評分；其輸出結構、重複執行一致性與契約檢查不能替代官方完整執行驗證。

本版尚未移植 Step3 的虛擬位元組額度重用及相應 WAR/WAW 依賴，也沒有精確模擬合併本地依賴、Pipe FIFO 和跨核 COPY 後的全域等待環。即使通過內部檢查，也不能據此保證官方模擬一定可執行。

本次編譯與檢查結果記錄於 `NON_SCORING_CHECKS_v3_1.md`。舊 `NON_SCORING_CHECKS.md`、`reports/validation_20261030.json` 與 `CHANGES_v3.md` 僅保留為 v0.3.0 歷史紀錄。

原始碼提交包仍只包含 `CMakeLists.txt`、`README.md` 與 `src/`；`references/`、官方評分器、資料、測試與本研究補充文件均不納入，提交程式不依賴它們。
