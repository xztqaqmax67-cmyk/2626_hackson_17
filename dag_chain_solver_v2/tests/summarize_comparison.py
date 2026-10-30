"""Create a portable result snapshot and human-readable comparison from measured rows."""
import argparse
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--report", type=Path, required=True)
p.add_argument("--root", type=Path, required=True)
a = p.parse_args()
report = json.loads(a.report.read_text(encoding="utf-8"))
rows = report["rows"]
assert len(rows) == 40 and len({(x["case"], x["cores"]) for x in rows}) == 40
assert all(x["valid"] and x["repeat_identical"] and x["v1_identical_to_archived"] for x in rows)
manifest = json.loads((a.root / "reports/v1_source_manifest.json").read_text(encoding="utf-8"))
old_root = a.root.parent / "dag_chain_solver"
assert all(hashlib.sha256((old_root / name).read_bytes()).hexdigest() == expected
           for name, expected in manifest.items()), "original source/documents changed"

def aggregate(items):
    return dict(count=len(items), valid=sum(x["valid"] for x in items),
                wins=sum(x["makespan"] < x["v1_makespan"] for x in items),
                ties=sum(x["makespan"] == x["v1_makespan"] for x in items),
                losses=sum(x["makespan"] > x["v1_makespan"] for x in items),
                v1_mean_speedup=sum(x["v1_speedup"] for x in items)/len(items),
                v2_mean_speedup=sum(x["speedup"] for x in items)/len(items),
                v1_solve_seconds=sum(x["v1_solve_seconds"] for x in items),
                v2_solve_seconds=sum(x["solve_seconds"] for x in items),
                v2_max_solve_seconds=max(x["solve_seconds"] for x in items),
                v1_ddr_bytes=sum(x["v1_data_movement_bytes"]["scheduled_copy_bytes"] for x in items),
                v2_ddr_bytes=sum(x["data_movement_bytes"]["scheduled_copy_bytes"] for x in items))

overall = aggregate(rows)
per_core = {str(n): aggregate([x for x in rows if x["cores"] == n]) for n in [2,3,4,5]}
summary = dict(date="2026-10-30", version="0.2.0", compiler="MSVC 19.44.35209.0 Release C++17",
               hardware=report["hardware"], baseline=report["baseline"],
               v1_score_reuse=report["v1_score_reuse"],
               v1_source_files_unchanged=len(manifest),
               executables={k: {"sha256": v["sha256"]} for k,v in report["executables"].items()},
               source_sha256={str(f.relative_to(a.root)).replace("\\", "/"):
                              hashlib.sha256(f.read_bytes()).hexdigest()
                              for f in sorted((a.root/"src").glob("*")) if f.is_file()},
               overall=overall, per_core=per_core, rows=rows)
(a.root / "reports/comparison_20261030.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
gain = (overall["v2_mean_speedup"] / overall["v1_mean_speedup"] - 1) * 100
ddr_change = (overall["v2_ddr_bytes"] / overall["v1_ddr_bytes"] - 1) * 100
lines = ["# DAG 鏈剖分 v2 測試結果", "", "日期：2026-10-30。這是本地評估，不是主辦方正式成績。", "",
         "## 方法", "",
         "使用相同的 10 個公開用例，各測 2、3、4、5 核，固定硬體參數與外部 C++ Scene B 評估器。兩版均為 MSVC 19.44 Release C++17 構建。",
         "原版每組重新生成方案，與既有已評估輸出逐位元組相同且輸入 SHA-256 相同，才沿用既有 Makespan 與搬運量；新版 40 組全部重新外部評估並重跑確認輸出一致。",
         f"原版 {len(manifest)} 份原始碼、測試與文件的 SHA-256 均保持不變。逐例數據與來源指紋見 [比較快照](reports/comparison_20261030.json)。", "",
         "加速比使用同一份本地 Scene A 單核基準：每例計算 `T1 / TN`，再取算術平均。這裡的 baseline 不是逐算子消融程式。", "",
         "## 平均加速比", "", "| 核心數 | 原版 v1 | 新版 v2 | 相對變化 |", "|---|---:|---:|---:|"]
for n, data in per_core.items():
    change = (data["v2_mean_speedup"]/data["v1_mean_speedup"]-1)*100
    lines.append(f"| {n} | {data['v1_mean_speedup']:.4f} | {data['v2_mean_speedup']:.4f} | {change:+.2f}% |")
lines.extend([f"| 全部 40 組 | {overall['v1_mean_speedup']:.4f} | {overall['v2_mean_speedup']:.4f} | {gain:+.2f}% |", "",
              f"兩版均為 40/40 有效。新版相對原版：**{overall['wins']} 組更快、{overall['ties']} 組相同、{overall['losses']} 組更慢**。", "",
              "## 發生變化的配置", "", "| 用例 | 核心數 | v1 cycles | v2 cycles | 完成時間變化 | v2 實際使用核心數 |", "|---|---:|---:|---:|---:|---:|"])
for x in rows:
    if x["makespan"] == x["v1_makespan"]:
        continue
    change = (x["makespan"]/x["v1_makespan"]-1)*100
    lines.append(f"| {x['case']} | {x['cores']} | {x['v1_makespan']:,.0f} | {x['makespan']:,.0f} | {change:+.2f}% | {x['active_cores']} |")
lines.extend(["", "完成時間變化為負表示縮短；未列出的配置與原版相同。退步仍可能來自快速估價與完整事件模擬的排序差異，不能將增加候選解讀為逐例改善保證。", "",
              "## 搬運量與求解耗時", "",
              f"40 組 DDR 搬運量合計由 {overall['v1_ddr_bytes']:,} 變為 {overall['v2_ddr_bytes']:,} bytes（變化 {ddr_change:+.2f}%）。這是搬運量總和，並非各用例額外搬運占比的平均；完成時間與搬運量可能呈現不同方向的變化。",
              f"同輪依序生成方案的耗時合計：v1 **{overall['v1_solve_seconds']:.2f} 秒**，v2 **{overall['v2_solve_seconds']:.2f} 秒**；v2 單次最長 **{overall['v2_max_solve_seconds']:.2f} 秒**。",
              "時間包含程式啟動及輸入輸出，不含編譯、外部評估與第二次重跑。同機可能有其他工作，屬觀測值；更大隱藏圖的 90 秒限制仍需驗收。", "",
              "## 重現與限制", "",
              "原始逐例方案與日誌位於 `runs/comparison_final/`，預設由 Git 忽略；精簡且可上傳 GitHub 的數據已保存於 `reports/comparison_20261030.json`。",
              "算法改動及重現命令見 [CHANGES_v2.md](CHANGES_v2.md)。主程式不讀這些報告，也不包含測試資料或外部評估器。",
              "內部估價仍未完整模擬 DDR 競爭、FIFO 和非同步記憶體重用；Linux 構建與執行尚待驗證。", ""])
validation_path = a.root / "reports/validation_20261030.json"
if validation_path.exists():
    validation = json.loads(validation_path.read_text(encoding="utf-8"))
    assert validation["msvc"]["executable_sha256"] == report["executables"]["v2"]["sha256"]
    peak = validation["mingw"]["observed_max_peak_working_set_bytes"]
    lines.extend(["## 補充驗證", "",
                  "- MSVC 與 MinGW 均通過算法回歸測試及命令列契約檢查。",
                  "- MinGW GCC 15.1 重新生成的 40 份方案與已評估的 MSVC 方案逐位元組一致。",
                  "- 20 個小型隨機 DAG 各測 2、5 核，共 40 組，全部通過外部評估與重複輸出檢查。",
                  "- case_07 四核及 case_08 五核用原 Python Scene B 重評，Makespan 與全部搬運量欄位一致；僅在記憶體中補充 PIPE_FIX 與 COPY 負值佔位。",
                  f"- MinGW 重跑時觀測到的程序峰值工作集最大為 {peak:,} bytes（約 {peak/1048576:.1f} MiB）；此值不是跨平台記憶體上限證明。",
                  "- 原始碼壓縮包的 6 個檔案均與工作目錄對應檔案一致，沒有編譯產物、測試資料或評估器。", "",
                  "檢查紀錄見 [驗證快照](reports/validation_20261030.json)。", ""])
(a.root / "EXPERIMENTS.md").write_text("\n".join(lines), encoding="utf-8")
print(json.dumps(dict(overall=overall, per_core=per_core), indent=2))
