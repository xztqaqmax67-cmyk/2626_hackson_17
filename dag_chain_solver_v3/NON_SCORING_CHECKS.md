# v3 非評分檢查紀錄

日期：2026-10-30。版本：`0.3.0`。依使用者要求，本次未呼叫任何外部評分器；
沒有計算 v3 的官方合法性、Makespan、相對 baseline 加速比或總分。

## 已完成

| 檢查 | 結果 |
| --- | --- |
| Windows x64 / MSVC 19.44 Release 編譯 | 通過 |
| Windows x64 / MinGW GCC 15.1 Release 編譯 | 通過 |
| `chain_invariants`、`parallel_jobs` | 兩套編譯器各 2/2 通過 |
| CLI 契約檢查 | 兩套編譯器均通過；涵蓋中文與空格路徑、參數、退出碼、輸入保護及輸出一致性 |
| v3 MSVC 對 v2 MSVC | 10 個輸入 × 2、3、4、5 核，40/40 JSON 逐位元組相同 |
| v3 MSVC 重複生成 | 同樣 40 組，同參數重跑的 JSON 全部相同 |
| v3 MinGW 對 v3 MSVC | 同樣 40 組，JSON 全部逐位元組相同 |
| 原 v2 檔案保留 | 建立 v3 前保存的 22 個檔案 SHA-256 全部未變 |
| 原估價模型保留 | `model.hpp` 與 `chain.hpp` 均與 v2 逐位元組相同 |

輸入使用研究目錄原有的 `hackathon_data/case_00.json` 至 `case_09.json`。
JSON 結構檢查包含頂層兩個欄位、計算算子覆蓋、核心列表數量及子圖排程不重複。
這些檢查能發現平行改寫造成的輸出變化，不能代替官方 Step 2 / Step 3 評估。

執行緒測試另外覆蓋：工作恰好執行一次、主執行緒參與、實際 worker 執行、
全部 join 後才傳遞例外、反轉例外到達順序仍按索引回報、建立失敗的同步回退，以及異常退出時回收已啟動執行緒。

## 資源觀察與測試界限

對 MSVC 的 40 次首次生成，每約 50 ms 取樣一次 Windows 程序資訊：

- 程序執行緒數最高觀察值為 **7**（包含程序內的執行期執行緒）；原始碼的搜尋計算執行緒上限為 **4**，含主執行緒。
- 觀察到的最高 `PeakWorkingSetSize` 為 **87,539,712 bytes，約 83.5 MiB**。

取樣可能漏掉短暫執行狀態；這不是任意輸入的資源上限證明。
腳本記錄了生成 JSON 的實際秒數，但這批檢查不是嚴格的效能基準測試，因此不據此宣稱多執行緒加速比。
本次未測試 Linux，也未執行 ThreadSanitizer。

## 結果位置與重現

- `reports/validation_20261030.json`：摘要、原始碼雜湊與保留檢查。
- `reports/output_checks_msvc_20261030.json`：v2/v3 一致性與 v3 重跑結果，含輸入、程式及方案 SHA-256。
- `reports/output_checks_mingw_20261030.json`：跨編譯器輸出比對結果。
- `runs/output_checks_msvc/`、`runs/output_checks_mingw/`：本機生成的 JSON；被 `.gitignore` 排除。
- `reports/v2_source_manifest.json`：改版前的 v2 檔案雜湊。

構建、`ctest`、CLI 及完整輸出比對命令見 `README.md`。
跨編譯器檢查使用 MinGW 程式重新生成同樣 40 組方案，與已通過 v2 對照的 MSVC JSON 比較。
也可將 `check_outputs.py` 的 `--reference` 指向 MSVC 版、`--solver` 指向 MinGW 版重現並額外重跑。

**下一步：收到官方評分器後，再進行正式合法性與品質比較。**
