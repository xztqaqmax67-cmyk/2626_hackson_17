# 原 problem2 評分器對照：2026-10-30 參考紀錄

使用數模比賽原 `problem2/official_code/` 程式，評估 `../report_v3_all/plans/` 中的既有四核方案。
此儲存庫中的原評分器位於 `../../前辈代码/problem2/`，程式碼保留原樣。

使用者指定配置衝突時以新版為準。主要結果為 `strict_matched`、`compat_matched`：
L1=1572864、UB=131072、DDR=60、同步=1000。舊配置測試僅保留為歷史對照。

| 測試組 | 輸入 | 結果 |
|---|---|---|
| strict_matched | 原資料，含 COPY cycles=-1 | 10 組均被舊版非負 cycles 校驗拒絕 |
| compat_matched | 副本僅 COPY cycles -1→0；新版配置 | case_00～06 單核／多核成功；case_07～09 不支援 PIPE_FIX |
| strict_original | 原資料、舊配置 | 同樣拒絕負數 COPY cycles |
| compat_original | cycles 相容副本、舊配置 | 7 組多核成功；case_05 單核在 120 秒超時；後 3 組不支援 PIPE_FIX |

原評分器會依搬運量計算 COPY 時間，將 sentinel -1 改成 0 並非將搬運設為零時間。
原始資料及方案不修改；這些副本測試不能稱為原新資料直接通過正式評分器。

成功的**同一批 7 組**平均加速比：原評分器 **2.3408667**，v3 **2.4798043**。
7 組 DDR 搬運量逐例相同；完整 10 例原版平均加速比保持 null。
新版配置的 case_05 單核評估約需 239 秒，建議 300 秒或更長超時。

先看 [current_config_comparison.csv](current_config_comparison.csv)，每例有兩邊周期、加速比、DDR 與差異。
`comparison.json/csv` 保存全部實驗組；各 case 的 `status.json` 保存結果、錯誤與評估耗時。
這裡的 `evaluation_seconds` 不是 solver 求解時間。

原版同步模型為 COPY_OUT 完成後等待 delay，才讓 COPY_IN 發射；v3 預設為原 producer 完成後 consumer 等待 1000 cycles。
原版 COPY 周期取整，核內排程與記憶體重用依賴也不同。因此相同配置數字不等於相同執行模型。

為便於 GitHub 下載，此目錄保留彙總、狀態、配置與相容性稽核，未提交約 260 MB 的原始時間線、
重複資料副本、機器絕對路徑命令與快取 manifest。可在上層重新執行：

```bash
python compare_original_problem2.py --stage compat_matched --timeout 300
```

新完整輸出位於 `../runs/original_problem2/`。`integrity.json` 是原測試時 36 個來源檔保持不變的歷史校驗，
不是本機 checkout 的即時校驗；Git 自動轉換換行也可能改變檔案 byte hash，而不改變 JSON 內容。
