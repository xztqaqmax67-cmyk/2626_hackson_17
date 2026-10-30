# Hackathon v3：可攜式工作目錄

**提交規範 v3.0 的獨立工程在 [submission/](submission/README.md)。** 本頁以下為保留的研究版；`solver.cpp`、`score.py`、`local_eval/` 不是新版提交入口。修改及 2～5 核重測見 [提交驗證報告](../SUBMISSION_REVIEW_20261030.md)。

先讀儲存庫根目錄的 [HANDOFF_zh.md](../HANDOFF_zh.md)。本目錄包含目前 C++ 求解器、
自製評分器、24 項測試、原版對照腳本及已完成的報告。
求解器與 `local_eval/` 的演算法檔案和已驗證版本相同；本次整理只調整執行腳本、資料位置與說明。

## 依賴與資料

- Python **3.10+**（建議 3.11）；無第三方套件需求。
- g++，支援 **C++17**。
- 測試資料共用儲存庫根目錄的 `../hackathon_data/`，不另存一份重複資料。
- 原數模程式位於 `../前辈代码/problem2/`。
- `config_LOCAL_ASSUMPTIONS.txt`：L1=1572864、UB=131072、DDR=60、同步=1000。

以下命令都從 `hackathon_v3/` 執行，與原開發機的絕對路徑無關。

## Windows

一鍵編譯、測試、評估全部資料：

```powershell
.\run_all_windows.ps1
```

只測兩例，或指定 Python：

```powershell
.\run_all_windows.ps1 -Cases case_01,case_07 -Out runs/smoke
.\run_all_windows.ps1 -Python 'C:\path\to\python.exe'
```

腳本優先尋找 PATH 中的 Python；原開發機上的 Blender Python 只是一項可選 fallback，其他電腦不需安裝 Blender。
若腳本政策阻擋，直接執行個別命令即可：

```powershell
g++ -O2 -std=c++17 -Wall -Wextra solver.cpp -o solver.exe
python -m unittest discover -s tests -v
python score.py --data ../hackathon_data --solver .\solver.exe --engine local -n 4 --solver-time-limit 1 --timeout 300 --out runs/local_v3
```

## Linux／WSL

```bash
bash run_all_linux.sh
```

或個別執行：

```bash
g++ -O2 -std=c++17 -Wall -Wextra solver.cpp -o solver
python3 -m unittest discover -s tests -v
python3 score.py --data ../hackathon_data --solver ./solver --engine local -n 4 --solver-time-limit 1 --timeout 300 --out runs/local_v3
```

## 評估已存方案與原版對照

使用 v3 評估已存方案（不重新求解，求解時間顯示 null）：

```bash
python score.py --data ../hackathon_data --plans report_v3_all/plans --out runs/existing_plans
```

原評分器直接測新資料，以及僅轉換 COPY cycles 格式的相容性測試：

```bash
python compare_original_problem2.py --stage strict_matched
python compare_original_problem2.py --stage compat_matched --timeout 300
```

此腳本預設讀取本目錄 `report_v3_all/plans/` 的已存四核方案與對應參考彙總，
不執行 solver；新報告存到 `runs/original_problem2/`。可用 `--cases case_01` 選一例。
若要比較新方案，應先明確更新方案與對應的 v3 報告，或調整腳本的參考報告位置。
跨電腦／更換輸入後使用新的 `--out`，不沿用舊機器的執行快取。

## 參考結果與模型限制

- [v3 十例彙總](report_v3_all/summary.json)：平均加速比 2.1303412，DDR 總量 2491414400 bytes。
- [同新版配置的原版對照](report_original_problem2/current_config_comparison.csv)：7 例可評估，3 例因 PIPE_FIX 不支援。
- [原版對照說明](report_original_problem2/README.md)：格式相容性、配置、同步語義與完整結果解讀。

`report_v3_all/`、`report_original_problem2/` 為歷史結果；數值不會因複製到本機就重新驗證。
實際耗時與軟預算搜尋結果會隨電腦、負載改變。新測試寫入 `runs/`，預設不加入 Git。

本地模型按原規則圖片解讀共享 DDR、原 producer→consumer 同步與記憶體容量，
保守固定寫入者同核、Pipe 優先序及虛擬 byte 重用依賴。它不是主辦方完整評估器。
原版的同步起點、周期取整、核內排序與寫回重用不同，因此兩者執行周期可以不同。
加速比採各例單核／多核比值的算術平均，求解時間不含編譯與評估；缺少百分制歸一化公式，不輸出官方總分。
