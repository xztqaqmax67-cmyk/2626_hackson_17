"""Verify all saved official v7 results and write 40-row comparison reports.

This reads existing plans/scores only and never runs a solver or evaluator.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys


MOVEMENT = ('original_graph_copy_bytes', 'scheduled_copy_bytes', 'added_copy_bytes',
            'partition_added_copy_bytes', 'spill_added_copy_bytes')


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def movement_check(value):
    assert set(MOVEMENT) <= set(value)
    assert value['scheduled_copy_bytes'] == value['original_graph_copy_bytes'] + value['added_copy_bytes']
    assert value['added_copy_bytes'] == value['partition_added_copy_bytes'] + value['spill_added_copy_bytes']


def rules_check(value, cores):
    assert value['scene'] == 'B' and value['num_cores'] == cores
    assert value['capacity_bytes'] == {'L1': 1572864, 'UB': 131072}
    assert value['bandwidth_bytes_per_cycle'] == 60 and value['cross_core_copy_delay_cycles'] == 1000
    movement_check(value['data_movement_bytes'])


def change(new, old):
    return 100 * (new / old - 1) if old else None


def geomean(values):
    values = list(values)
    return math.exp(statistics.mean(math.log(v) for v in values))


def summarize(rows, comparison='v5'):
    def total(key):
        return sum(r[key] for r in rows)
    prefix = comparison + '_'
    result = dict(row_count=len(rows), mean_speedup=statistics.mean(r['speedup'] for r in rows),
                  geomean_speedup=geomean(r['speedup'] for r in rows), cycles=total('cycles'),
                  copy_bytes=total('copy_bytes'), spill_copy_bytes=total('spill_copy_bytes'),
                  generation_seconds=total('generation_seconds'),
                  generation_mean_seconds=statistics.mean(r['generation_seconds'] for r in rows),
                  generation_min_seconds=min(r['generation_seconds'] for r in rows),
                  generation_max_seconds=max(r['generation_seconds'] for r in rows),
                  data_movement_bytes={k: sum(r['data_movement_bytes'][k] for r in rows) for k in MOVEMENT})
    result[prefix + 'mean_speedup'] = statistics.mean(r[prefix + 'speedup'] for r in rows)
    result[prefix + 'geomean_speedup'] = geomean(r[prefix + 'speedup'] for r in rows)
    result[prefix + 'generation_mean_seconds'] = statistics.mean(r[prefix + 'generation_seconds'] for r in rows)
    for key in ('cycles', 'copy_bytes', 'spill_copy_bytes'):
        result[prefix + key] = total(prefix + key)
        result[key + '_sum_change_percent'] = change(result[key], result[prefix + key])
        result[key + '_improved'] = sum(r[key] < r[prefix + key] for r in rows)
        result[key + '_unchanged'] = sum(r[key] == r[prefix + key] for r in rows)
        result[key + '_regressed'] = sum(r[key] > r[prefix + key] for r in rows)
    result['mean_speedup_change_percent'] = change(result['mean_speedup'], result[prefix + 'mean_speedup'])
    return result


def main():
    root = Path(__file__).resolve().parents[1]
    old_root = root.parent / 'dag_chain_solver_v5'
    v6_root = root.parent / 'dag_chain_solver_v6'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, default=root / 'runs/official_all')
    parser.add_argument('--solver', type=Path, default=root / 'build_msvc/bin/npu_sched.exe')
    args = parser.parse_args()
    run = args.run.resolve(); reference = root / 'references/hackathon_20261030'
    relative = lambda p: Path(os.path.relpath(p, root)).as_posix()
    measured_path = run / 'summary.json'
    baseline_path = root / 'reports/v5_results_baseline.json'
    old_scored_path = old_root / 'runs/final/official_problem2/summary.json'
    old_timing_path = old_root / 'runs/final/output_checks_msvc/report.json'
    v6_path = v6_root / 'reports/FIVE_CORES_15S.json'
    measured, baseline, old_scored, old_timing, v6 = map(read, (measured_path, baseline_path, old_scored_path, old_timing_path, v6_path))
    assert measured['completed'] and measured['version'] == '0.7.0'
    assert measured['core_limits'] == [2, 3, 4, 5] and measured['search_mode'] == 'fixed_work'
    assert measured['requested_budget_seconds'] is None and len(measured['rows']) == 40
    assert baseline['version'] == old_scored['version'] == old_timing['version'] == '0.5.0'
    assert sha(old_scored_path) == baseline['source_sha256']['v5_official_summary']
    assert old_timing['solver_executable_sha256'] == old_scored['generator_executable_sha256']
    assert v6['completed'] and v6['version'] == '0.6.1' and v6['requested_budget_seconds'] == 15
    assert sha(baseline_path) == v6['provenance_sha256']['v5_report']
    expected = {(f'case_{i:02}', n) for n in (2, 3, 4, 5) for i in range(10)}
    current = {(r['case'], r['cores']): r for r in measured['rows']}
    v5rows = {(r['case'], r['cores']): r for r in baseline['rows'] if r['cores'] > 1}
    old_scores = {(r['case'], r['cores']): r for r in old_scored['rows']}
    old_times = {(r['case'], r['cores']): r for r in old_timing['rows']}
    v6rows = {(r['case'], r['cores']): r for r in v6['rows']}
    assert set(current) == set(v5rows) == set(old_scores) == set(old_times) == expected
    assert set(v6rows) == {(f'case_{i:02}', 5) for i in range(10)}
    assert all(r['status'] == 'ok' and r['exit_code'] == 0 for r in measured['rows'])
    manifest = read(root / 'reports/rules_manifest.json')
    for name, digest in manifest['files'].items():
        assert sha(reference / name) == digest, ('Changed official file', name)
    config_sha = sha(reference / 'data/config.txt')
    assert config_sha == measured['config_sha256'] == old_scored['config_sha256'] == baseline['config']['sha256'] == v6['config']['sha256']
    assert measured['official_source_sha256'] == old_scored['evaluator_source_sha256'] == v6['official_source_sha256']
    for name, digest in measured['official_source_sha256'].items():
        assert sha(reference / name) == digest
    for name, digest in measured['source_sha256'].items():
        assert sha(root / name) == digest, ('Changed measured source', name)
    assert sha(args.solver.resolve()) == measured['solver_executable_sha256']
    rows = []
    for key in sorted(expected, key=lambda k: (k[1], k[0])):
        case, cores = key; r = current[key]; old = v5rows[key]; old_s = old_scores[key]; old_t = old_times[key]
        plan_path = root / r['plan']; result_path = root / r['result']; graph = reference / f'data/{case}.json'
        old_plan = old_root / old['plan']; old_result_path = old_root / old['source_result']
        single_path = root / f'reports/singlecore_baseline/{case}.json'
        assert r['input_sha256'] == old_s['input_sha256'] == old_t['input_sha256'] == sha(graph)
        assert r['plan_sha256'] == sha(plan_path) and r['result_sha256'] == sha(result_path)
        assert old_s['plan_sha256'] == old_t['plan_sha256'] == sha(old_plan)
        assert old_s['result_sha256'] == sha(old_result_path)
        assert old_s['status'] == 'ok' and old_s['exit_code'] == 0
        score, old_score, single, plan = map(read, (result_path, old_result_path, single_path, plan_path))
        rules_check(score, cores); rules_check(old_score, cores)
        assert single['num_cores'] == 1 and single['makespan'] == r['singlecore_cycles'] == old['singlecore_cycles']
        assert score['makespan'] == r['cycles'] and score['data_movement_bytes'] == r['data_movement_bytes']
        assert old_score['makespan'] == old['cycles'] == old_s['makespan']
        assert old_score['data_movement_bytes'] == old_s['data_movement_bytes']
        assert old['copy_bytes'] == old_score['data_movement_bytes']['scheduled_copy_bytes']
        assert old['spill_copy_bytes'] == old_score['data_movement_bytes']['spill_added_copy_bytes']
        assert len(plan['core_schedules']) == cores
        speedup = single['makespan'] / score['makespan']
        assert math.isclose(speedup, r['speedup'], rel_tol=1e-12)
        assert math.isclose(old['speedup'], single['makespan'] / old['cycles'], rel_tol=1e-12)
        generation = r['generation']; movement = score['data_movement_bytes']
        assert math.isfinite(generation['seconds']) and generation['seconds'] > 0
        row = dict(case=case, cores=cores, used_compute_cores=sum(bool(s) for s in plan['core_schedules']),
                   status='ok', singlecore_cycles=single['makespan'], cycles=score['makespan'], speedup=speedup,
                   copy_bytes=movement['scheduled_copy_bytes'], spill_copy_bytes=movement['spill_added_copy_bytes'],
                   data_movement_bytes=movement, generation_seconds=generation['seconds'], evaluation_seconds=r['evaluation_seconds'],
                   v5_cycles=old['cycles'], v5_speedup=old['speedup'], v5_copy_bytes=old['copy_bytes'],
                   v5_spill_copy_bytes=old['spill_copy_bytes'], v5_data_movement_bytes=old_score['data_movement_bytes'],
                   v5_generation_seconds=old_t['generation']['seconds'],
                   cycles_change_vs_v5_percent=change(score['makespan'], old['cycles']),
                   copy_change_vs_v5_percent=change(movement['scheduled_copy_bytes'], old['copy_bytes']),
                   spill_change_vs_v5_percent=change(movement['spill_added_copy_bytes'], old['spill_copy_bytes']),
                   observed_peak_working_set_bytes=generation['observed_peak_working_set_bytes'],
                   sampled_max_process_threads=generation['sampled_max_process_threads'],
                   memory_peak_by_core=score['memory_peak_by_core'], input_sha256=r['input_sha256'],
                   plan_sha256=r['plan_sha256'], result_sha256=r['result_sha256'],
                   singlecore_result_sha256=sha(single_path), v5_result_sha256=sha(old_result_path),
                   artifacts=dict(input=relative(graph), plan=relative(plan_path), result=relative(result_path),
                                  trace=relative(plan_path.parent / 'trace.json'), log=relative(plan_path.parent / 'log.txt'),
                                  singlecore_baseline=relative(single_path), v5_plan=relative(old_plan), v5_result=relative(old_result_path)))
        if cores == 5:
            previous = v6rows[key]
            old6_plan = v6_root / previous['artifacts']['plan']; old6_result = v6_root / previous['artifacts']['result']
            assert previous['input_sha256'] == r['input_sha256']
            assert previous['plan_sha256'] == sha(old6_plan) and previous['result_sha256'] == sha(old6_result)
            previous_score = read(old6_result); rules_check(previous_score, 5)
            assert previous_score['makespan'] == previous['cycles'] and previous_score['data_movement_bytes'] == previous['data_movement_bytes']
            assert previous['singlecore_cycles'] == single['makespan']
            assert math.isclose(previous['speedup'], single['makespan'] / previous['cycles'], rel_tol=1e-12)
            row.update(v6_cycles=previous['cycles'], v6_speedup=previous['speedup'], v6_copy_bytes=previous['copy_bytes'],
                       v6_spill_copy_bytes=previous['data_movement_bytes']['spill_added_copy_bytes'],
                       v6_data_movement_bytes=previous['data_movement_bytes'], v6_generation_seconds=previous['generation_seconds'],
                       cycles_change_vs_v6_percent=change(row['cycles'], previous['cycles']),
                       copy_change_vs_v6_percent=change(row['copy_bytes'], previous['copy_bytes']))
            row['artifacts'].update(v6_plan=relative(old6_plan), v6_result=relative(old6_result))
        rows.append(row)
    by_cores = {str(n): summarize([r for r in rows if r['cores'] == n]) for n in (2, 3, 4, 5)}
    summary = summarize(rows)
    v6comparison = summarize([r for r in rows if r['cores'] == 5], comparison='v6')
    report = dict(version='0.7.0', completed=True, row_count=40, core_limits=[2, 3, 4, 5], search_mode='fixed_work',
                  requested_budget_seconds=None, paths_relative_to='dag_chain_solver_v7/', config=baseline['config'],
                  definitions=dict(speedup='Official single-core cycles / plan cycles; arithmetic and geometric means of ratios are reported separately.',
                                   cycles='Official simulated cycles including movement/synchronization, not generator process wall time.',
                                   copy='scheduled_copy_bytes; original + signed partition delta + spill. 1 MiB = 1048576 bytes.',
                                   generation='One serial process wall-time measurement per case, including input/output, excluding separate scoring.',
                                   comparison='v5 fixed-work historical runs; n5 additionally compared to historical v6 15-second runs. Different budgets/strategies, not a controlled CPU-speed benchmark.',
                                   cores='Available core limit; solver may use fewer cores.',
                                   sums='Sums of independent cases, not one combined workload.',
                                   retention='All 40 new plans retained; no per-case fallback to historical plans.'),
                  source_sha256=measured['source_sha256'], solver_executable_sha256=measured['solver_executable_sha256'],
                  official_source_sha256=measured['official_source_sha256'],
                  provenance_sha256=dict(measured_summary=sha(measured_path), v5_report=sha(baseline_path),
                                         v5_scoring_summary=sha(old_scored_path), v5_timing_report=sha(old_timing_path),
                                         v6_report=sha(v6_path), rules_manifest=sha(root / 'reports/rules_manifest.json')),
                  artifacts=dict(measured_summary=relative(measured_path), v5_report=relative(baseline_path),
                                 v5_scoring_summary=relative(old_scored_path), v5_timing_report=relative(old_timing_path), v6_report=relative(v6_path)),
                  verification=dict(source_executable_input_plan_result_hashes=True, historical_results_verified=True,
                                    official_config_and_code_match=True, official_successful_plans=40),
                  summary=summary, by_cores=by_cores, five_cores_vs_v6=v6comparison, rows=rows)
    reports = root / 'reports'
    (reports / 'RESULTS_2_TO_5_CORES.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# v7：2～5 核全部 40 組官方測試', '',
             '40 個新方案全部通過未修改的官方 Scene B 評分器。先串行生成方案，再執行評分；本次預設使用固定搜尋配額，沒有 15／30 秒牆鐘目標。',
             '加速比 = 官方單核 cycles ÷ 本方案 cycles；可用核數是上限，方案可以留下空核。搬運量是官方 scheduled_copy_bytes，並非生成器的執行時間。',
             '下列相對 v5 的百分比為 (v7 / v5 − 1) × 100%，負值代表減少。算術平均是各用例加速比的平均，另列幾何平均。', '',
             '| 可用核數 | v5 平均加速比 | v7 平均加速比 | v7 幾何平均 | cycles 合計變化 | COPY 合計變化 | 溢出 COPY 合計變化 | cycles 改善/持平/退步 | 平均生成秒 |',
             '|---:|---:|---:|---:|---:|---:|---:|---|---:|']
    for n, s in by_cores.items():
        lines.append(f'| {n} | {s["v5_mean_speedup"]:.6f}× | {s["mean_speedup"]:.6f}× | {s["geomean_speedup"]:.6f}× | {s["cycles_sum_change_percent"]:+.3f}% | {s["copy_bytes_sum_change_percent"]:+.3f}% | {s["spill_copy_bytes_sum_change_percent"]:+.3f}% | {s["cycles_improved"]}/{s["cycles_unchanged"]}/{s["cycles_regressed"]} | {s["generation_mean_seconds"]:.3f} |')
    lines += ['', f'全部 40 組算術平均加速比：v5 {summary["v5_mean_speedup"]:.6f}× → v7 {summary["mean_speedup"]:.6f}×；cycles 改善／持平／退步 {summary["cycles_improved"]}／{summary["cycles_unchanged"]}／{summary["cycles_regressed"]}。']
    for n in (2, 3, 4, 5):
        lines += ['', f'## {n} 核', '', '| 用例 | 單核基準 cycles | v7 cycles | 加速比 | 總 COPY bytes | 溢出 COPY bytes | 生成秒 | cycles 相對 v5 | COPY 相對 v5 |',
                  '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
        for r in (x for x in rows if x['cores'] == n):
            lines.append(f'| {r["case"]} | {r["singlecore_cycles"]:,} | {r["cycles"]:,} | {r["speedup"]:.6f}× | {r["copy_bytes"]:,} | {r["spill_copy_bytes"]:,} | {r["generation_seconds"]:.3f} | {r["cycles_change_vs_v5_percent"]:+.3f}% | {r["copy_change_vs_v5_percent"]:+.3f}% |')
    s = v6comparison
    lines += ['', '## 五核與先前 v6 約 15 秒版本比較', '',
              f'平均加速比：v6 {s["v6_mean_speedup"]:.6f}× → v7 {s["mean_speedup"]:.6f}×；cycles 合計變化 {s["cycles_sum_change_percent"]:+.3f}%，COPY 合計變化 {s["copy_bytes_sum_change_percent"]:+.3f}%，溢出 COPY 合計變化 {s["spill_copy_bytes_sum_change_percent"]:+.3f}%。',
              'v6 採歷史單次約 15 秒結果，v7 採本次固定工作量結果；搜尋策略、預算與測量時間均不同，不能用本表隔離純程式執行效率。', '',
              '| 用例 | v6 cycles | v7 cycles | cycles 變化 | v6 COPY bytes | v7 COPY bytes | v6 生成秒 | v7 生成秒 |',
              '|---|---:|---:|---:|---:|---:|---:|---:|']
    for r in (x for x in rows if x['cores'] == 5):
        lines.append(f'| {r["case"]} | {r["v6_cycles"]:,} | {r["cycles"]:,} | {r["cycles_change_vs_v6_percent"]:+.3f}% | {r["v6_copy_bytes"]:,} | {r["copy_bytes"]:,} | {r["v6_generation_seconds"]:.3f} | {r["generation_seconds"]:.3f} |')
    lines += ['', '## 搬運分類（bytes）', '',
              '總 COPY = 原圖 COPY + 分核淨新增 + 溢出新增。保留官方定義，分核淨新增可為負值。下表完整列出 40 組，原始 JSON 保留未四捨五入數值。', '',
              '| 核數 | 用例 | 原圖 COPY | 分核淨新增 | 溢出新增 | 總 COPY |', '|---:|---|---:|---:|---:|---:|']
    for r in rows:
        m = r['data_movement_bytes']
        lines.append(f'| {r["cores"]} | {r["case"]} | {m["original_graph_copy_bytes"]:,} | {m["partition_added_copy_bytes"]:,} | {m["spill_added_copy_bytes"]:,} | {r["copy_bytes"]:,} |')
    lines += ['', '## 重現與檔案', '',
              '完整生成及官方評分：`python scripts/run_v7_official.py`；只重整報表：`python scripts/summarize_v7.py`。可使用 `--stage generate`／`--stage evaluate` 分開階段；已有結果需來源雜湊一致才可續跑。',
              f'本次方案、result、trace、log：`{relative(run)}/case_XX_nN/`。所有精確數值、雜湊和各筆路徑：[RESULTS_2_TO_5_CORES.json](RESULTS_2_TO_5_CORES.json)。',
              '本次各點生成時間只量測一次，包含輸入、輸出與程序收尾，不包含評分器；固定候選數不代表執行秒數固定。資源取樣間隔約 50 ms，不能保證捕捉所有瞬時峰值。',
              '本報表只驗證這 40 組方案與歷史比較資料，不代表任意圖的形式化正確性證明，也不推定其他單元測試已通過。', '']
    (reports / 'RESULTS_2_TO_5_CORES_zh.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(dict(summary=summary, by_cores=by_cores, five_cores_vs_v6=v6comparison), indent=2), flush=True)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
