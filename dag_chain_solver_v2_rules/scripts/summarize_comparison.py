"""Summarize the completed controlled generator benchmark and official plan comparison."""
import hashlib
import json
from pathlib import Path
import shutil
import statistics


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    v3 = root.parent/'dag_chain_solver_v3'
    unrestricted = json.loads((root/'runs/thread_benchmark_20261030/benchmark.json').read_text(encoding='utf-8'))
    measured = json.loads((root/'runs/thread_benchmark_performance_cores_20261030/benchmark.json').read_text(encoding='utf-8'))
    official = json.loads((root/'runs/official_comparison_20261030/summary.json').read_text(encoding='utf-8'))
    resources = json.loads((root/'runs/resource_observations_20261030/report.json').read_text(encoding='utf-8'))
    previous = json.loads((v3/'reports/official_evaluation_v3_1_20261030.json').read_text(encoding='utf-8'))
    assert unrestricted['completed'] and measured['completed'] and official['completed']
    assert len(measured['rows'])==40 and official['valid']==official['matching_v3']==40
    quality = statistics.mean(row['speedup_over_supplied_singlecore'] for row in official['rows'])
    assert abs(quality-previous['mean_speedup'])<1e-12
    total = measured['overall']
    validation = dict(serial_version='0.2.1',parallel_version='0.3.1',date='2026-10-30',
                      tests=dict(msvc_ctest_passed=3,mingw_ctest_passed=3,cli_both_compilers=True,
                                 new_input_contracts_per_compiler=26,official_valid=40,official_matches_v3=40,
                                 configurations_with_identical_outputs=40,identical_generations=800),
                      generator_speedup=total['speedup'],generator_time_saved_percent=total['saved_percent'],
                      algorithm_quality_speedup_over_supplied_singlecore=quality,
                      quality_unchanged=True,benchmark=measured,unrestricted_benchmark=unrestricted,resources=resources,
                      source_sha256={p.relative_to(root).as_posix():sha(p)
                                     for p in [root/'CMakeLists.txt',*sorted((root/'src').glob('*'))] if p.is_file()})
    reports = root/'reports'
    reports.mkdir(exist_ok=True)
    (reports/'comparison_20261030.json').write_text(json.dumps(validation,indent=2),encoding='utf-8')
    for source, target in [('thread_benchmark_20261030/benchmark.json','thread_benchmark_20261030.json'),
                           ('thread_benchmark_performance_cores_20261030/benchmark.json','thread_benchmark_performance_cores_20261030.json'),
                           ('official_comparison_20261030/summary.json','official_comparison_20261030.json'),
                           ('resource_observations_20261030/report.json','resource_observations_20261030.json')]:
        shutil.copyfile(root/'runs'/source,reports/target)
    lines = ['# 新規則下 v2 單執行緒與 v3 多執行緒比較', '',
             '日期：2026-10-30。v2_rules `0.2.1` 與 v3 `0.3.1` 使用相同候選、估價、修復與固定合併順序；',
             '差別為有效核心數 2..N 的搜尋依序執行或平行執行。原 `dag_chain_solver_v2` 保留。', '',
             f"**在同一組高效能核心上的受控對照，整體生成速度提升 {total['speedup']:.4f}×，生成時間減少 {total['saved_percent']:.2f}%。**",
             f"40 組各取中位數後，單執行緒合計 {total['serial_sum_medians_seconds']:.4f} 秒，",
             f"多執行緒合計 {total['parallel_sum_medians_seconds']:.4f} 秒。這些是生成排程 JSON 的實際時間。", '',
             '## 各可用核心數的求解耗時', '',
             '| NPU 可用核心數 | v3 搜尋執行緒上限（含主） | v2 十例合計中位數（秒） | v3 十例合計中位數（秒） | 生成速度倍數 | 省時 |',
             '| --- | --- | --- | --- | --- | --- |']
    for n, value in measured['by_cores'].items():
        lines.append(f"| {n} | {int(n)-1} | {value['serial_sum_medians_seconds']:.4f} | "
                     f"{value['parallel_sum_medians_seconds']:.4f} | {value['speedup']:.4f}× | {value['saved_percent']:.2f}% |")
    raw = unrestricted['overall']
    lines += ['', '## 一般排程與受控排程', '',
              '| 測試條件 | v2 合計中位數（秒） | v3 合計中位數（秒） | 生成速度倍數 |',
              '| --- | --- | --- | --- |',
              f"| 一般排程，未限制核心 | {raw['serial_sum_medians_seconds']:.4f} | {raw['parallel_sum_medians_seconds']:.4f} | {raw['speedup']:.4f}× |",
              f"| 同一組高效能核心，主比較 | {total['serial_sum_medians_seconds']:.4f} | {total['parallel_sum_medians_seconds']:.4f} | {total['speedup']:.4f}× |", '',
              '首輪出現明顯耗時波動及個別比值高於搜尋執行緒數，因此另做受控重測；不刪除或取代原始樣本。',
              '這些現象可能包含處理器排程、背景負載或其他系統因素，沒有把差異全部歸因於大小核切換。',
              f"受控輪使用 Windows 回報較高 EfficiencyClass 的邏輯核心：{measured['affinity']['selected_logical_processors']}。",
              '這是相同 8 個高效能核心的可用集合，並非獨占核心；背景負載與頻率變化仍可能影響耗時。',
              '兩個版本都繼承相同核心集合；只設定專屬測試程序及其子程序，不改動產品程式或其他應用程式。']
    lines += ['', 'N=2 只有一項搜尋工作，兩版都由主執行緒完成；其差異應視為實測波動或執行開銷，不歸因於平行化。',
              '小用例可能沒有收益甚至稍慢，程式啟動、排程與執行緒建立成本佔比更高。', '',
              '## 公平性與統計方法', '',
              f"- CPU：{measured['hardware'].get('cpu_name',measured['hardware']['processor'])}；{measured['hardware']['logical_processors']} 個邏輯處理器。",
              f"- 系統：{measured['hardware']['os']}；電源模式保持 Turbo，沒有改動程序優先權；受控輪的核心集合如上。",
              '- 兩版使用 MSVC 19.44 x64 Release 和相同最佳化旗標。MinGW 另外通過功能測試，不混入本表計時。',
              '- `model.hpp`、`chain.hpp` 逐位元組相同；`chain.cpp` 搜尋主體相同；主程式 I/O 除版本字串外相同。',
              '- 每組各暖機一次，接著量測四對，順序為單→多、多→單、多→單、單→多。',
              '- 計時包含讀圖、搜尋、寫出 JSON 及程序啟動／結束；不包含官方評分、JSON 雜湊檢查或資源取樣。',
              '- 每次僅啟動一個生成器，期間沒有同時編譯或評分；沒有控制使用者其他背景應用程式。',
              '- 主指標為各組中位數加總後相除，避免微小用例的比值主導平均。所有原始時間、配對次序及範圍都保存。',
              '- 每次生成都核對原先官方評分通過的 v3 方案雜湊：兩輪共 800 次（含暖機）全部相同。', '',
              '## 官方方案品質', '',
              'cycles 與搬運量的逐例、分核數及 baseline 對照見 [詳細比較](CYCLES_AND_TRAFFIC_COMPARISON.md)。', '',
              '新 v2 的 40 組方案另行重跑原包問題 2 評分器，**40/40 通過**。',
              '與 v3 既有結果逐項比對，排除輸入方案檔名後，完整結果（含 Makespan、搬運量和時間線）40/40 相同。',
              f'兩版相對同包單核基準的平均 NPU 排程加速比均為 **{quality:.6f}×**。',
              '因此本次多執行緒收益是減少生成方案的等待時間，沒有改變方案評分。單核基準及配置亦保持相同。', '',
              '## 額外資源觀察', '',
              '在正式計時全部結束後，對兩個較大用例的 5 核搜尋另以約 50 ms 取樣。',
              '下表的執行緒是 Windows 程序總數，包含執行期執行緒；不是演算法 worker 數。取樣不保證捕捉任意輸入的瞬時最大值。', '',
              '| 用例 | 版本 | 觀察到的程序執行緒上限 | PeakWorkingSet（MiB） |', '| --- | --- | --- | --- |']
    for row in resources['rows']:
        observation = row['observations']
        lines.append(f"| {row['case']} | {row['version']} | {observation['sampled_max_process_threads']} | "
                     f"{observation['observed_peak_working_set_bytes']/1048576:.2f} |")
    lines += ['', '## 逐例生成耗時中位數', '',
              '| 用例 | 核數 | v2 秒 | v3 秒 | v2/v3 |', '| --- | --- | --- | --- | --- |']
    for row in measured['rows']:
        lines.append(f"| {row['case']} | {row['cores']} | {row['serial_median_seconds']:.6f} | "
                     f"{row['parallel_median_seconds']:.6f} | {row['speedup']:.4f}× |")
    lines += ['', '## 產物與重現', '',
              '- `reports/comparison_20261030.json`：完整摘要、硬體、來源／執行檔雜湊與原始量測。',
              '- `reports/thread_benchmark_20261030.json`：一般排程的 400 次生成資料。',
              '- `reports/thread_benchmark_performance_cores_20261030.json`：同核心集合的 400 次生成資料。',
              '- `reports/official_comparison_20261030.json`：40 組官方結果與 v3 比對。',
              '- `runs/thread_benchmark_20261030/`：逐組單／多執行緒 JSON。',
              '- `runs/official_comparison_20261030/`：原包評分結果、Trace、日誌、命令與雜湊。',
              '- 先執行 `scripts/benchmark_threads.py`，結束後加 `--performance-cores --out runs/thread_benchmark_performance_cores_20261030` 執行受控輪。',
              '- 兩輪計時全部結束後，依序執行 `scripts/evaluate_comparison.py`、`scripts/observe_resources.py`、`scripts/summarize_comparison.py`。',
              '- 重新計時需選新的 `--out` 目錄，勿將前後不同執行混成一批。',
              '- `dist/dag_chain_solver_v2_rules.zip` 是只有 CMake、README 和 src 的單執行緒提交包，不含官方資料或評分器。', '',
              '受控方法依據 [Microsoft PROCESSOR_RELATIONSHIP 文件](https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-processor_relationship)',
              '及 [SetProcessAffinityMask 文件](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask)：較高 EfficiencyClass 表示較高效能，子程序繼承處理器 affinity。', '',
              '結果限於本機與這 10 個公開用例；不是隱藏用例或其他 CPU 上的速度保證。', '']
    (root/'THREADING_COMPARISON.md').write_text('\n'.join(lines),encoding='utf-8')
    print(json.dumps(dict(overall=total,by_cores=measured['by_cores'],quality=quality),indent=2))


if __name__ == '__main__':
    main()
