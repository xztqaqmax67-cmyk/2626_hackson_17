"""Verify saved 15-second official results and compare with historical 30-second runs.

Read-only with respect to sources, plans and scores. This never runs a solver or scorer.
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
    assert set(MOVEMENT) <= set(value), 'Missing COPY classification'
    assert value['scheduled_copy_bytes'] == value['original_graph_copy_bytes'] + value['added_copy_bytes']
    assert value['added_copy_bytes'] == value['partition_added_copy_bytes'] + value['spill_added_copy_bytes']


def rules_check(value):
    assert value['scene'] == 'B' and value['num_cores'] == 5
    assert value['capacity_bytes'] == {'L1': 1572864, 'UB': 131072}
    assert value['bandwidth_bytes_per_cycle'] == 60
    assert value['cross_core_copy_delay_cycles'] == 1000
    movement_check(value['data_movement_bytes'])


def change(new, old):
    return 100 * (new / old - 1)


def main():
    root = Path(__file__).resolve().parents[1]
    history_root = root.parent / 'dag_chain_solver_v5_30s'
    snapshot = root / 'snapshots/v6_0_6_0_before_perf_15s'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, default=root / 'runs/five_15s')
    parser.add_argument('--solver', type=Path, default=root / 'build_msvc/bin/npu_sched.exe')
    args = parser.parse_args()
    run = args.run.resolve()
    measured_path = run / 'summary.json'
    old_path = history_root / 'reports/FIVE_CORES_30S.json'
    baseline_path = root / 'reports/v5_results_baseline.json'
    measured, history, baseline = read(measured_path), read(old_path), read(baseline_path)
    assert measured['completed'] and measured['version'] == '0.6.1'
    assert measured['core_limit'] == 5 and measured['requested_budget_seconds'] == 15
    assert history['completed'] and history['version'] == '0.5.1'
    assert history['core_limit'] == 5 and history['requested_budget_seconds'] == 30
    assert baseline['version'] == '0.5.0'
    assert sha(baseline_path) == history['source_sha256']['v5_baseline_report']
    expected = {f'case_{i:02}' for i in range(10)}
    assert len(measured['rows']) == len(history['rows']) == 10
    current = {r['case']: r for r in measured['rows']}
    previous = {r['case']: r for r in history['rows']}
    v5 = {r['case']: r for r in baseline['rows'] if r['cores'] == 5}
    assert set(current) == set(previous) == set(v5) == expected
    assert all(r['status'] == 'ok' and r['exit_code'] == 0 for r in current.values()), 'Do not exclude failed evaluations'
    assert all(r['status'] == 'ok' for r in previous.values())
    relative = lambda p: Path(os.path.relpath(p, root)).as_posix()
    reference = root / 'references/hackathon_20261030'
    manifest = read(root / 'reports/rules_manifest.json')
    assert sha(root / 'reports/rules_manifest.json') == history['source_sha256']['rules_manifest']
    for name, digest in manifest['files'].items():
        assert sha(reference / name) == digest, f'Changed rule/input file: {name}'
    assert measured['config_sha256'] == history['config']['sha256'] == baseline['config']['sha256'] == sha(reference / 'data/config.txt')
    assert measured['official_source_sha256'] == history['official_source_sha256']
    for name, digest in measured['official_source_sha256'].items():
        assert sha(reference / name) == digest
    for name, digest in measured['source_sha256'].items():
        assert sha(root / name) == digest, f'Changed measured source: {name}'
    assert sha(args.solver.resolve()) == measured['solver_executable_sha256'], 'Measured executable changed'
    # Historical 0.5.1 and the saved pre-optimization v6 0.6.0 differ only in
    # the CLI version string inside src/. Keep this distinct from a new run.
    historical_source = {name: digest for name, digest in history['source_sha256'].items()
                         if name.startswith('src/')}
    for name, digest in historical_source.items():
        assert sha(history_root / name) == digest, f'Changed historical source: {name}'
        if name == 'src/main.cpp':
            assert (snapshot / name).read_text(encoding='utf-8').replace('0.6.0', '0.5.1') == (history_root / name).read_text(encoding='utf-8')
        else:
            assert sha(snapshot / name) == digest, f'Unexpected pre-change source: {name}'
    rows = []
    for case in sorted(expected):
        r, old, original = current[case], previous[case], v5[case]
        graph = reference / f'data/{case}.json'
        plan_path = root / r['plan']
        result_path = plan_path.parent / 'result.json'
        old_plan = history_root / old['artifacts']['plan']
        old_result_path = history_root / old['artifacts']['result']
        single_path = root / f'reports/singlecore_baseline/{case}.json'
        assert r['input_sha256'] == old['input_sha256'] == sha(graph)
        assert r['plan_sha256'] == sha(plan_path)
        assert r['result_sha256'] == sha(result_path)
        assert old['plan_sha256'] == sha(old_plan)
        assert old['result_sha256'] == sha(old_result_path)
        assert old['singlecore_result_sha256'] == sha(single_path)
        score, old_score, single = read(result_path), read(old_result_path), read(single_path)
        rules_check(score)
        rules_check(old_score)
        assert single['num_cores'] == 1
        single_cycles = single['makespan']
        assert single_cycles == r['singlecore_cycles'] == old['singlecore_cycles'] == original['singlecore_cycles']
        assert score['makespan'] == r['cycles'] and old_score['makespan'] == old['cycles']
        assert score['data_movement_bytes'] == r['data_movement_bytes']
        assert old_score['data_movement_bytes'] == old['data_movement_bytes']
        assert old['copy_bytes'] == old_score['data_movement_bytes']['scheduled_copy_bytes']
        assert r['v5_cycles'] == old['v5_cycles'] == original['cycles']
        assert r['v5_copy_bytes'] == old['v5_copy_bytes'] == original['copy_bytes']
        speedup = single_cycles / score['makespan']
        assert math.isclose(speedup, r['speedup'], rel_tol=1e-12)
        assert math.isclose(single_cycles / old['cycles'], old['speedup'], rel_tol=1e-12)
        generation = r['generation']
        assert math.isfinite(generation['seconds']) and generation['seconds'] > 0
        assert math.isfinite(old['generation_seconds']) and old['generation_seconds'] > 0
        plan = read(plan_path)
        assert len(plan['core_schedules']) == 5
        movement = score['data_movement_bytes']
        rows.append(dict(
            case=case, cores=5, used_compute_cores=sum(bool(s) for s in plan['core_schedules']),
            status='ok', generation_seconds=generation['seconds'], evaluation_seconds=r['evaluation_seconds'],
            old30_generation_seconds=old['generation_seconds'], singlecore_cycles=single_cycles,
            cycles=score['makespan'], speedup=speedup, copy_bytes=movement['scheduled_copy_bytes'],
            data_movement_bytes=movement, old30_cycles=old['cycles'], old30_speedup=old['speedup'],
            old30_copy_bytes=old['copy_bytes'], old30_data_movement_bytes=old['data_movement_bytes'],
            v5_cycles=original['cycles'], v5_speedup=original['speedup'], v5_copy_bytes=original['copy_bytes'],
            cycles_change_vs_30s_percent=change(score['makespan'], old['cycles']),
            copy_change_vs_30s_percent=change(movement['scheduled_copy_bytes'], old['copy_bytes']),
            speedup_change_vs_30s_percent=change(speedup, old['speedup']),
            cycles_change_vs_v5_percent=change(score['makespan'], original['cycles']),
            observed_peak_working_set_bytes=generation['observed_peak_working_set_bytes'],
            sampled_max_process_threads=generation['sampled_max_process_threads'],
            memory_peak_by_core=score['memory_peak_by_core'], input_sha256=r['input_sha256'],
            plan_sha256=r['plan_sha256'], result_sha256=r['result_sha256'],
            old30_plan_sha256=old['plan_sha256'], old30_result_sha256=old['result_sha256'],
            artifacts=dict(input=relative(graph), plan=relative(plan_path), result=relative(result_path),
                           trace=relative(plan_path.parent / 'trace.json'), log=relative(plan_path.parent / 'log.txt'),
                           singlecore_baseline=relative(single_path), old30_plan=relative(old_plan),
                           old30_result=relative(old_result_path))))
    summary = dict(
        mean_speedup=statistics.mean(r['speedup'] for r in rows),
        old30_mean_speedup=statistics.mean(r['old30_speedup'] for r in rows),
        v5_mean_speedup=statistics.mean(r['v5_speedup'] for r in rows),
        generation_mean_seconds=statistics.mean(r['generation_seconds'] for r in rows),
        generation_min_seconds=min(r['generation_seconds'] for r in rows),
        generation_max_seconds=max(r['generation_seconds'] for r in rows),
        old30_generation_mean_seconds=statistics.mean(r['old30_generation_seconds'] for r in rows),
        max_observed_peak_working_set_bytes=max(r['observed_peak_working_set_bytes'] for r in rows),
        max_sampled_process_threads=max(r['sampled_max_process_threads'] for r in rows),
        **{key: sum(r[key] for r in rows) for key in
           ['cycles', 'copy_bytes', 'old30_cycles', 'old30_copy_bytes', 'v5_cycles', 'v5_copy_bytes']},
        data_movement_bytes={key: sum(r['data_movement_bytes'][key] for r in rows) for key in MOVEMENT})
    for measure, old_name in [('cycles', 'old30_cycles'), ('copy_bytes', 'old30_copy_bytes')]:
        label = 'cycles' if measure == 'cycles' else 'copy'
        summary[f'{label}_improved_vs_30s'] = sum(r[measure] < r[old_name] for r in rows)
        summary[f'{label}_unchanged_vs_30s'] = sum(r[measure] == r[old_name] for r in rows)
        summary[f'{label}_regressed_vs_30s'] = sum(r[measure] > r[old_name] for r in rows)
    summary.update(
        mean_speedup_change_vs_30s_percent=change(summary['mean_speedup'], summary['old30_mean_speedup']),
        cycles_sum_change_vs_30s_percent=change(summary['cycles'], summary['old30_cycles']),
        copy_sum_change_vs_30s_percent=change(summary['copy_bytes'], summary['old30_copy_bytes']))
    report = dict(
        version='0.6.1', core_limit=5, requested_budget_seconds=15, completed=True, row_count=10,
        paths_relative_to='dag_chain_solver_v6/', config=baseline['config'],
        definitions=dict(
            speedup='Official single-core cycles divided by new plan cycles; mean is arithmetic mean of ten ratios.',
            cycles='Official simulated completion cycles, including movement/synchronization; not generator runtime.',
            generation='One saved process wall-time measurement per case, including parsing/cache creation/output; scoring is separate.',
            history='0.5.1 historical 30-second run; src algorithm matches saved pre-change v6 0.6.0 except version string.',
            comparison='15 versus 30 seconds and historical runs do not isolate pure implementation speed; use PERF_15S fixed-work benchmark for that.',
            copy='scheduled_copy_bytes; classifications retain official signed partition delta; 1 MiB=1048576 bytes.',
            sums='Sums of independent cases, not one combined workload.',
            cores='Five cores available; schedules may leave cores empty.',
            repeatability='One measurement per case. Timed cutoff can yield different plans on rerun.',
            retention='All ten actual new plans included, no per-case fallback to old plans.'),
        source_sha256=measured['source_sha256'], solver_executable_sha256=measured['solver_executable_sha256'],
        official_source_sha256=measured['official_source_sha256'],
        provenance_sha256=dict(measured_summary=sha(measured_path), historical30_report=sha(old_path),
                               v5_report=sha(baseline_path), rules_manifest=sha(root / 'reports/rules_manifest.json')),
        artifacts=dict(measured_summary=relative(measured_path), historical30_report=relative(old_path),
                       v5_report=relative(baseline_path), fixed_work_report='reports/PERF_15S.json'),
        verification=dict(source_executable_input_plan_result_hashes=True, official_config_and_code_match=True,
                          historical30_algorithm_matches_prechange_v6=True, official_successful_plans=10),
        summary=summary, rows=rows)
    reports = root / 'reports'
    (reports / 'FIVE_CORES_15S.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# v6 0.6.1：五核、約 15 秒搜尋結果', '',
             '本次 10 個方案全部通過未修改的官方 Scene B 評分器；先串行生成所有方案，再統一評分。',
             '對照為先前保存的 0.5.1／30 秒方案，其演算法與修改前的 v6 0.6.0 相同；版本改名沒有重跑歷史評分。', '',
             f'平均生成時間 **{summary["generation_mean_seconds"]:.3f} 秒**，範圍 {summary["generation_min_seconds"]:.3f}～{summary["generation_max_seconds"]:.3f} 秒。',
             f'相對官方單核的平均加速比：原 v5 **{summary["v5_mean_speedup"]:.6f}×**，歷史 30 秒版 **{summary["old30_mean_speedup"]:.6f}×**，本次 15 秒版 **{summary["mean_speedup"]:.6f}×**。',
             f'相對 30 秒版，cycles 改善／持平／退步：**{summary["cycles_improved_vs_30s"]}／{summary["cycles_unchanged_vs_30s"]}／{summary["cycles_regressed_vs_30s"]}**；cycles 合計變化 {summary["cycles_sum_change_vs_30s_percent"]:+.3f}%，搬運合計變化 {summary["copy_sum_change_vs_30s_percent"]:+.3f}%。', '',
             '加速比 = 官方單核 cycles ÷ 本方案 cycles，平均為十個比值的算術平均。五核是可用上限，可有空核。', '',
             '| 用例 | 生成秒 | cycles | 加速比 | COPY bytes | 相對 30 秒 cycles 變化 | 相對 30 秒 COPY 變化 |',
             '|---|---:|---:|---:|---:|---:|---:|']
    for r in rows:
        lines.append(f'| {r["case"]} | {r["generation_seconds"]:.3f} | {r["cycles"]:,} | {r["speedup"]:.6f}× | {r["copy_bytes"]:,} | {r["cycles_change_vs_30s_percent"]:+.3f}% | {r["copy_change_vs_30s_percent"]:+.3f}% |')
    lines += ['', '搬運分類直接取官方結果，單位 bytes；分核淨新增可能為負，保留原值。', '',
              '| 用例 | 原圖 COPY | 分核淨新增 | 溢出新增 | 總 COPY |',
              '|---|---:|---:|---:|---:|']
    for r in rows:
        m = r['data_movement_bytes']
        lines.append(f'| {r["case"]} | {m["original_graph_copy_bytes"]:,} | {m["partition_added_copy_bytes"]:,} | {m["spill_added_copy_bytes"]:,} | {r["copy_bytes"]:,} |')
    lines += ['',
              f'觀察到的最大程序 working set：{summary["max_observed_peak_working_set_bytes"] / 1048576:.3f} MiB；最多取樣到 {summary["max_sampled_process_threads"]} 條程序執行緒，包含執行期執行緒。50 ms 取樣不能保證捕捉所有瞬時峰值。', '',
              '**這份比較同時改變了實作效率與搜尋預算，而且 30 秒結果來自歷史單次測量，不能用它隔離純程式效率收益。**',
              '固定工作量的 CPU 效率另見 [PERF_15S_zh.md](PERF_15S_zh.md)；本表用來確認縮短預算後實際得到的方案品質。',
              '每點只測一次，保留所有改善與退步；牆鐘截止會受機器負載影響，沒有宣稱重跑方案相同。初始種子搜尋仍需完整完成，15 秒不是任意大圖的硬截止保證。', '',
              '未在本腳本推定其他測試是否通過；本報表只驗證這 10 個官方結果與來源雜湊。',
              f'新方案／result／trace／log 位於 `{relative(run)}/case_XX/`；精確數值與每筆檔案位置見 [FIVE_CORES_15S.json](FIVE_CORES_15S.json)。',
              f'歷史 30 秒結果：`{relative(old_path)}`。重新整理報表使用 `python scripts/summarize_five_15s.py`，不會重新生成或評分。', '']
    (reports / 'FIVE_CORES_15S_zh.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
