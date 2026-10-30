# DAG 鏈剖分實驗版

GitHub 讀者與 AI 接手可先讀 [簡要導讀](OVERVIEW_zh.md)，了解算法、單核 baseline 加速比及後續優化方向。導讀為研究文件，不納入比賽提交包。

這是獨立的 C++17 方案生成器，實驗「把較晚完成的前驅與後繼連成鏈，再分段分核」的想法。
主程式只讀輸入圖、輸出標準方案；不載入配置、呼叫外部程序、輸出評分或依賴目錄外的程式。
它使用內部啟發式估價選解；完整合法性和 Makespan 由外部場景 B 評估器驗證。

## 規則依據

工程及介面按 `材料/提交要求文档.md` v3.0（2026-10-30）實作：

- C++17、CMake 3.16+，預設只建立 `build/bin/npu_sched`（Windows 為 `.exe`）。
- `npu_sched <graph.json> -n <2..5> -o <plan.json>`；預設 4 核。
- 接受 `--cores=4`、`--output=path`；未知參數拒絕。不接受 `--config` 或搜尋時間參數。
- 固定 L1=1572864 bytes、UB=131072 bytes、DDR=60 bytes/cycle、跨核等待=1000 cycles。
- 依備註保留 `PIPE_FIX` 為獨立第五條 Pipe。原始 COPY 的 `cycles=-1` 可直接讀入。
- 跨核估價採 COPY_OUT 結束後等待 1000 cycles，再開始 COPY_IN。
- 固定參數網格、固定迭代數、明確平局次序，單執行緒；不以時鐘或亂數決定搜尋結果。
- 不採用舊圖片中的 40/30/30 評分。官方單核基準未提供，不自行宣稱官方加速比或總分。

備註中的「10 例共 5 分鐘」和新版文件的「每次 90 秒」是不同口徑；實測需同時記錄。
程式以固定候選數控制工作量；對更大圖會依節點、邊與 Tensor 數減少候選數。
這不是任意規模輸入的 90 秒最壞情況證明，正式驗收仍需在目標環境測量時間和記憶體。

## 構建及執行

Linux：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched /path/to/graph.json -n 4 -o /path/to/plan.json
```

Windows / Visual Studio：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe ..\hackathon_data\case_00.json -n 4 -o runs\case_00_n4.json
```

Windows / MinGW + Ninja：

```powershell
cmake -S . -B build_mingw -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_mingw -j
.\build_mingw\bin\npu_sched.exe ..\hackathon_data\case_00.json --cores=4 --output=runs\case_00_n4.json
```

各生成器使用不同 build 目錄。支援中文及含空格路徑；未指定輸出時，寫到輸入圖旁的
`<case>_multicore_res.json`。方案只有 `node_to_subgraph`、`core_schedules`，不含估價或分數。
退出碼為 0 成功、1 內部錯誤、2 命令列錯誤、3 輸入錯誤、4 輸出錯誤、5 無可用候選。

## 算法

1. **圖索引。** 排除 DDR COPY 分配對象，保留經 COPY 的依賴，計算拓撲序、上游累積工作量
   和下游關鍵路徑。所有原始依賴均保留。
2. **鏈覆蓋。** 第一類候選為每個算子選累積完成時間最大的前驅；另一類加入通訊代價偏好。
   同一前驅只保留一條鏈內後繼，優先延伸下游關鍵工作較大的分支，形成互不重疊的路徑覆蓋。
   未選中的依賴仍是鏈間依賴，沒有被刪除。
3. **鏈段。** 依各 Pipe 工作量切開過重的路徑，嘗試三種分段尺度。保守地把同一 Tensor 的
   多個寫入者放在同核；這可能合併不同鏈段，屬算法策略而非新增賽規。
4. **分核。** 重鏈段先分配，試算五條 Pipe 的負載平方增量、重複讀取、跨核搬運與同步成本。
   分組只用來決定核心，不把整個組強行合成一個輸出子圖，避免縮點後的依賴環。
5. **核內排序。** 在原算子 DAG 的 ready 集合中，用時間草圖、關鍵路徑／DFS 次序及記憶體
   生命週期代價選擇下一個算子。輸出時一個算子一個子圖，同核不同鏈可交錯。
6. **搜尋。** 加入逐算子貪心及單核候選作為基線；最多六種鏈配置、兩輪鏈段遷移。
   遷移先由增量搬運帳本篩選，再重新排序及估價。保留內部估價最佳的可用候選。

內部估價包含各 Pipe 草圖、總 DDR 流量下界與最晚下次使用的溢出近似。
它不重現完整共享頻寬事件模擬，也不是正式分數。估價較好不保證外部 Makespan 較好。
特別排除估計中會在同一 Tensor 的多次寫入之間換出的排序，因參考 Step 2 的重載改邊
可能使這類排序在 Step 3 死鎖。這是候選防護，不能替代完整外部驗證。

`chain_baseline` 是同一模型下移除鏈候選及鏈段遷移的消融對照，並不是原 `前辈代码_cpp`
完整搜尋器。兩者候選數及求解時間不同，結果用來觀察新增鏈搜尋的效果，不能宣稱等預算勝出。

## 程式結構及來源

- `src/chain.cpp`：新實作的鏈覆蓋、分段、鏈段分核、遷移與候選選擇。
- `src/model.hpp`：從本庫 `hackathon_v3/solver.cpp` 抽取圖讀取、排序和啟發式估價，
  移除配置讀取、舊搜尋及隨機遷移；修正同步位置、逐目的核心寫回計量及部分寫入防護。
- `src/main.cpp`：固定命令列契約、UTF-8／Windows 寬字元入口、標準方案寫出。
- `tests/`：開發用不變量、命令列及外部評估測試，預設不構建。
- `scripts/package.py`：按白名單打包獨立原始碼。

第三方庫：**無**。JSON reader 為本庫既有的自製實作；執行期僅依賴 C++17 標準庫，
Windows 路徑／檔案替換 API 已用 `_WIN32` 隔離。沒有官方評估器副本。

## 開發測試

```powershell
cmake -S . -B build -A x64 -DCHAIN_BUILD_TESTS=ON
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
python tests/contracts.py --solver build/bin/npu_sched.exe --out runs/contracts
python tests/verify.py --solver build/bin/npu_sched.exe --baseline build/bin/chain_baseline.exe --evaluator <外部parity_runner.exe> --data ../hackathon_data --out runs/verification --repeat
```

`verify.py` 預設測全部輸入的 2、3、4、5 核。外部 evaluator 使用現有 C++ 場景 B 移植版的
測試入口，以固定新配置評估原始輸入。所有圖與評估器都只由測試腳本讀取，不隨包提交。
`--repeat` 會重跑並逐位元組比較方案；可用 `--cores 4 --cases case_01 case_07` 限定範圍。
測試報告中的 `solve_seconds` 不含外部評估耗時，後者另列 `evaluation_seconds`。
原版 Python 對照需要在測試記憶體中補充第五條 Pipe 及 COPY 負值佔位，不改寫原始檔。

## 打包

```powershell
python scripts/package.py --team T0123
```

`dist/T0123.zip` 僅包含頂層 CMake、README 及 `src/`，沒有 build、測試輸入、官方評估器、
預生成方案或報告。壓縮包中的預設構建不需要 Python 或本庫其他目錄。

此版本是研究原型；驗證結果與限制另外記錄在工作目錄的 `EXPERIMENTS.md`，不納入提交包。
