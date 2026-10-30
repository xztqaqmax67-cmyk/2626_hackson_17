# DAG 鏈剖分排程器：v2 新規則對照版

版本 **0.2.1**，目錄為 `dag_chain_solver_v2_rules`。原 `dag_chain_solver_v2` 保留未修改。
本版把新題目包要求回移到單執行緒搜尋，供與 v3 `0.3.1` 公平比較多執行緒的效果。

本版與 v3 使用相同的圖模型、候選生成、估價、排序修復及候選選擇方式；只有最外層
「有效核心數 2 到 N 的搜尋」依序執行。`model.hpp`、`chain.hpp` 與 v3 位元組相同；
`chain.cpp` 的核心演算法亦相同，僅外層 `solve()` 改回同步迴圈。
兩輪比較共 800 次生成均與 v3 排程相同；本版 40 組官方評估也全部通過，
Makespan、搬運量與完整時間線均與 v3 一致。兩版相對附帶單核基準的平均排程加速比都是 1.775120×。

## 新規則

- 使用 L1=1572864 bytes、UB=131072 bytes、DDR=60 bytes/cycle、跨核 COPY 等待=1000 cycles。
  依據新包 `data/config.txt` 及 C++ 提交要求；不採用部分說明文件殘留的舊數值。
- 溢出估價採 `floor(硬容量 × 90%)` 軟門檻，每個操作先配置所有輸入與輸出，再換出閒置資料。
  當前操作需要的 Tensor 不可換出；無合法 victim 但仍在硬容量內時，允許短暫超過軟門檻。
- 部分寫入 Tensor 換出後，由新版官方 Step2 正確處理後續寫者與讀者的重載版本。
  求解器不再僅因此拒絕候選；仍嘗試集中寫入以降低搬運，並在每次寫入後讓舊 DDR 備份失效。
- 同一 Tensor 的所有計算寫者必須同核。生成候選時綁定寫者，最終輸出前再次檢查。
- 拒絕重複邊、重複 JSON 鍵、共享 ID 衝突及原始完整圖的環；`op.pipe` 必須是五種有效 Pipe 之一。

## 演算法流程

1. 讀取圖，保留經 COPY 的依賴，建立拓撲序、關鍵路徑與寫者群組。
2. 依前驅累積完成時間和通訊成本連成 DAG 路徑，將較重路徑拆為鏈段。
3. 把鏈段配置到核心，平衡各 Pipe 負載與搬運成本；同一 Tensor 的寫者共同配置。
4. 在原始算子 DAG 上生成核內順序，讓不同鏈的操作交錯；輸出以每個計算操作作為一個子圖。
5. 加入貪心、單核、不同鏈參數、有限輪次遷移及寫者排序候選，以相同估價模型比較。
6. 對有效核心數 `2..N` **依序**搜尋；每次搜尋本身含單核退路。按與 v3 相同的固定次序比較各組結果，
   選出的方案可以少用核心，輸出以空列表補足 N 核。

本程式沒有工作執行緒、亂數停止或依執行時間停止的搜尋。
內部估價仍是近似模型，沒有重現官方共享 DDR 事件、Step3 虛擬容量 WAR/WAW 或完整 Pipe FIFO 等待環。
方案品質與完整可執行性由官方評分器驗證；求解器本身不輸出分數。

## 編譯及執行

需要 C++17、CMake 3.16+。第三方函式庫：**無**。提交程式不讀取配置檔、評分器或其他目錄。

Linux：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/npu_sched graph.json -n 4 -o plan.json
```

Windows / Visual Studio：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release -j
.\build\bin\npu_sched.exe graph.json -n 4 -o plan.json
```

固定介面為 `npu_sched <graph.json> -n <2..5> -o <plan.json>`，也接受 `--cores=4`、`--output=plan.json`。
核心數預設為 4，省略輸出路徑時寫入輸入圖旁的 `<輸入檔名>_multicore_res.json`。
`--version` 顯示 `0.2.1`；不提供 `--config`、`--help` 或額外搜尋參數。

## 開發測試與比較

完整研究目錄提供測試，最小提交包不包含它們：

```sh
cmake -S . -B build-dev -DCMAKE_BUILD_TYPE=Release -DCHAIN_BUILD_TESTS=ON
cmake --build build-dev --config Release -j
ctest --test-dir build-dev -C Release --output-on-failure
python tests/contracts.py --solver build-dev/bin/npu_sched --out runs/contracts
python tests/rules_contracts.py --solver build-dev/bin/npu_sched --out runs/rules_contracts
```

Windows 使用對應的 `.exe` 路徑。這些測試檢查演算法不變量、新規則與 CLI，不執行官方評分。
MSVC 與 MinGW 均通過 3 項單元測試、原有 CLI 與 26 個輸入契約案例。
在相同高效能核心集合、MSVC Release 下，40 組中位數合計：本版 74.41 秒、v3 37.59 秒；
v3 生成速度約為本版 1.98 倍，省時約 49.5%。這是本機公開用例結果，仍可能受背景負載影響。
完整方法、一般排程原始結果、逐例數據與官方對照見研究目錄中的 `THREADING_COMPARISON.md`。
比較時應使用相同編譯器和 Release 設定、相同輸入與參數，避免同時執行兩個求解器或評分器干擾計時。
NPU Makespan 的加速比與求解器多執行緒的牆鐘加速比是不同指標。

## 原始碼提交包

```sh
python scripts/package.py --team dag_chain_solver_v2_rules
```

產生 `dist/dag_chain_solver_v2_rules.zip`，只含 `CMakeLists.txt`、本 README 及 `src/`。
不包含官方資料、評分器、預生成方案、測試或編譯產物；預設構建無需 Python 或本研究目錄其他文件。
