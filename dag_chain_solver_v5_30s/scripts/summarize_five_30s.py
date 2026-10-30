"""Verify saved five-core measurements and report them; never generate or score."""

import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import sys


MIB = 1024 * 1024
MOVEMENT_KEYS = (
    'original_graph_copy_bytes', 'scheduled_copy_bytes', 'added_copy_bytes',
    'partition_added_copy_bytes', 'spill_added_copy_bytes',
)


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha256(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check_hash(path, expected):
    require(sha256(path) == expected, f'SHA256 mismatch: {path}')


def check_movement(movement):
    require(set(MOVEMENT_KEYS) <= set(movement), 'Missing movement classifications')
    require(movement['scheduled_copy_bytes'] == movement['original_graph_copy_bytes'] +
            movement['added_copy_bytes'], 'COPY total does not equal original + added')
    require(movement['added_copy_bytes'] == movement['partition_added_copy_bytes'] +
            movement['spill_added_copy_bytes'], 'Added COPY does not equal partition + spill')
    # Partition deltas may be negative when original COPY operations disappear.
    # Preserve that official definition instead of clamping it to zero.


def change_percent(new, old):
    return 100.0 * (new / old - 1) if old else (0.0 if new == 0 else None)


def main():
    root = Path(__file__).resolve().parents[1]
    previous = root.parent / 'dag_chain_solver_v5'
    reference = root / 'references/hackathon_20261030'
    summary_path = root / 'runs/five_30s/summary.json'
    baseline_path = root / 'reports/v5_results_baseline.json'
    timings_path = previous / 'runs/final/output_checks_msvc/report.json'
    previous_official_path = previous / 'runs/final/official_problem2/summary.json'
    manifest_path = root / 'reports/rules_manifest.json'
    measured = read_json(summary_path)
    baseline = read_json(baseline_path)
    timings = read_json(timings_path)
    previous_official = read_json(previous_official_path)
    manifest = read_json(manifest_path)
    expected = {f'case_{i:02d}' for i in range(10)}
    relative = lambda path: Path(os.path.relpath(path, root)).as_posix()

    # Never select only successful rows or substitute an old plan. A partial or
    # failed run remains in the harness summary and cannot become a success report.
    require(measured.get('completed') is True, 'The measured run is not complete')
    require(len(measured['rows']) == 10 and {r['case'] for r in measured['rows']} == expected,
            'Expected all ten cases exactly once')
    require(all(r['status'] == 'ok' and r['exit_code'] == 0 and r['cores'] == 5
                for r in measured['rows']), 'Official failures must not be omitted')
    require(measured['version'] == '0.5.1' and baseline['version'] == timings['version'] ==
            previous_official['version'] == '0.5.0', 'Unexpected versions')
    require(measured['core_limit'] == 5 and measured['requested_budget_seconds'] == 30.0,
            'Unexpected measured core limit or time budget')
    require(len(baseline['rows']) == 50 and len({(r['case'], r['cores']) for r in baseline['rows']}) == 50,
            'Expected the complete pinned v5 table')
    old = {r['case']: r for r in baseline['rows'] if r['cores'] == 5}
    single = {r['case']: r for r in baseline['rows'] if r['cores'] == 1}
    old_times = {r['case']: r for r in timings['rows'] if r['cores'] == 5}
    old_scores = {r['case']: r for r in previous_official['rows'] if r['cores'] == 5}
    require(set(old) == set(single) == set(old_times) == set(old_scores) == expected,
            'Missing original v5 comparison records')
    require(timings['solver_executable_sha256'] == previous_official['generator_executable_sha256'],
            'Historical timings and scores came from different executables')
    check_hash(previous_official_path, baseline['source_sha256']['v5_official_summary'])
    require(measured['config_sha256'] == baseline['config']['sha256'] ==
            previous_official['config_sha256'], 'Different rule configurations')
    require(measured['official_source_sha256'] == previous_official['evaluator_source_sha256'],
            'Different official evaluator sources')
    for name, digest in manifest['files'].items():
        check_hash(reference / name, digest)
    check_hash(reference / 'data/config.txt', measured['config_sha256'])
    for name, digest in measured['official_source_sha256'].items():
        check_hash(reference / name, digest)
    for name, digest in measured['source_sha256'].items():
        check_hash(root / name, digest)
    check_hash(root / 'build_msvc/bin/npu_sched.exe', measured['solver_executable_sha256'])

    rows = []
    for measured_row in sorted(measured['rows'], key=lambda row: row['case']):
        case = measured_row['case']
        prior = old[case]
        prior_time = old_times[case]
        prior_score = old_scores[case]
        graph_path = reference / 'data' / f'{case}.json'
        plan_path = root / measured_row['plan']
        result_path = plan_path.parent / 'result.json'
        old_plan_path = previous / prior['plan']
        old_result_path = previous / prior['source_result']
        single_path = root / single[case]['source_result']
        result = read_json(result_path)
        old_result = read_json(old_result_path)
        single_result = read_json(single_path)
        require(measured_row['input_sha256'] == prior_time['input_sha256'] == prior_score['input_sha256'],
                f'{case}: historical and new input hashes differ')
        check_hash(graph_path, measured_row['input_sha256'])
        check_hash(plan_path, measured_row['plan_sha256'])
        check_hash(result_path, measured_row['result_sha256'])
        require(prior_time['plan_sha256'] == prior_score['plan_sha256'], f'{case}: historical plan mismatch')
        check_hash(old_plan_path, prior_time['plan_sha256'])
        check_hash(old_result_path, prior_score['result_sha256'])
        require(sha256(single_path) == sha256(previous / single[case]['source_result']),
                f'{case}: copied single-core baseline changed')
        require(prior_score['status'] == 'ok' and prior_score['exit_code'] == 0,
                f'{case}: historical evaluation failed')
        for score in (result, old_result):
            require(score['scene'] == 'B' and score['num_cores'] == 5 and
                    score['capacity_bytes'] == {'L1': 1572864, 'UB': 131072} and
                    score['bandwidth_bytes_per_cycle'] == 60 and score['cross_core_copy_delay_cycles'] == 1000,
                    f'{case}: unexpected evaluator rules')
        require(single_result['num_cores'] == 1 and single_result['makespan'] == single[case]['cycles'] ==
                prior['singlecore_cycles'] == measured_row['singlecore_cycles'], f'{case}: baseline mismatch')
        require(result['makespan'] == measured_row['cycles'] and
                old_result['makespan'] == prior['cycles'] == prior_score['makespan'] == measured_row['v5_cycles'],
                f'{case}: saved score mismatch')
        movement = result['data_movement_bytes']
        old_movement = old_result['data_movement_bytes']
        check_movement(movement)
        check_movement(old_movement)
        require(movement == measured_row['data_movement_bytes'] and old_movement == prior_score['data_movement_bytes'],
                f'{case}: movement summary mismatch')
        require(old_movement['scheduled_copy_bytes'] == prior['copy_bytes'] == measured_row['v5_copy_bytes'],
                f'{case}: historical COPY mismatch')
        single_cycles = single_result['makespan']
        cycles = result['makespan']
        speedup = single_cycles / cycles
        v5_speedup = single_cycles / prior['cycles']
        require(math.isclose(speedup, measured_row['speedup'], rel_tol=1e-12) and
                math.isclose(v5_speedup, prior['speedup'], rel_tol=1e-12), f'{case}: speedup mismatch')
        generation = measured_row['generation']
        old_generation = prior_time['generation']
        require(math.isfinite(generation['seconds']) and generation['seconds'] > 0 and
                math.isfinite(old_generation['seconds']) and old_generation['seconds'] > 0,
                f'{case}: invalid generation timing')
        plan = read_json(plan_path)
        require(len(plan['core_schedules']) == 5, f'{case}: incorrect available core count')
        artifacts = dict(input=relative(graph_path), plan=relative(plan_path), result=relative(result_path),
                         trace=relative(plan_path.parent / 'trace.json'), log=relative(plan_path.parent / 'log.txt'),
                         singlecore_baseline=relative(single_path), v5_plan=relative(old_plan_path),
                         v5_result=relative(old_result_path), v5_trace=relative(old_result_path.parent / 'trace.json'))
        require(all((root / name).is_file() for name in artifacts.values()), f'{case}: missing original artifacts')
        rows.append(dict(
            case=case, cores=5, used_compute_cores=sum(bool(schedule) for schedule in plan['core_schedules']),
            status='ok', generation_seconds=generation['seconds'],
            v5_historical_generation_seconds=old_generation['seconds'], evaluation_seconds=measured_row['evaluation_seconds'],
            singlecore_cycles=single_cycles, cycles=cycles, speedup=speedup,
            copy_bytes=movement['scheduled_copy_bytes'], copy_MiB=movement['scheduled_copy_bytes'] / MIB,
            data_movement_bytes=movement, v5_cycles=prior['cycles'], v5_speedup=v5_speedup,
            v5_copy_bytes=prior['copy_bytes'], v5_copy_MiB=prior['copy_bytes'] / MIB,
            v5_data_movement_bytes=old_movement,
            cycles_change_percent=change_percent(cycles, prior['cycles']),
            speedup_change_percent=change_percent(speedup, v5_speedup),
            copy_change_percent=change_percent(movement['scheduled_copy_bytes'], prior['copy_bytes']),
            observed_peak_working_set_bytes=generation['observed_peak_working_set_bytes'],
            sampled_max_process_threads=generation['sampled_max_process_threads'],
            memory_peak_by_core=result['memory_peak_by_core'],
            input_sha256=measured_row['input_sha256'], plan_sha256=measured_row['plan_sha256'],
            result_sha256=measured_row['result_sha256'], v5_plan_sha256=prior_time['plan_sha256'],
            v5_result_sha256=prior_score['result_sha256'], singlecore_result_sha256=sha256(single_path), artifacts=artifacts))

    # Read recorded test evidence only. This script must not run a solver or scorer.
    validation_path = root / 'runs/validation_30s/msvc_validation_commands.json'
    synthetic_path = root / 'runs/validation_30s/synthetic_msvc/report.json'
    validation = read_json(validation_path)
    synthetic = read_json(synthetic_path)
    require(validation['completed'] and len(validation['rows']) == 3 and
            all(row['exit_code'] == 0 for row in validation['rows']), 'Incomplete MSVC contract tests')
    require(any(row['script'] == 'rules_contracts.py' and '26 graph contract checks passed' in row['stdout']
                for row in validation['rows']), 'Missing 26 passing graph contracts')
    require(synthetic['completed'] and synthetic['passed'] == len(synthetic['rows']) == 64 and synthetic['failed'] == 0
            and all(row['status'] == 'ok' for row in synthetic['rows']), 'Incomplete synthetic official checks')
    require(validation['solver_executable_sha256'] == synthetic['solver_executable_sha256'], 'Quick test binary mismatch')
    ctests = {}
    for compiler, folder in [('MSVC', 'build_msvc'), ('MinGW', 'build_mingw')]:
        log_path = root / folder / 'Testing/Temporary/LastTest.log'
        log_text = log_path.read_text(encoding='utf-8', errors='replace')
        require(log_text.count('Test Passed.') == 7 and 'Test Failed.' not in log_text,
                f'{compiler}: expected seven passed CTests')
        ctests[compiler] = dict(passed=7, failed=0, source=relative(log_path), sha256=sha256(log_path))
    totals = {name: sum(row[name] for row in rows) for name in ('singlecore_cycles', 'cycles', 'v5_cycles', 'copy_bytes', 'v5_copy_bytes')}
    summary = dict(
        **totals, mean_speedup=statistics.mean(row['speedup'] for row in rows),
        v5_mean_speedup=statistics.mean(row['v5_speedup'] for row in rows),
        improved=sum(row['cycles'] < row['v5_cycles'] for row in rows),
        unchanged=sum(row['cycles'] == row['v5_cycles'] for row in rows),
        regressed=sum(row['cycles'] > row['v5_cycles'] for row in rows),
        copy_improved=sum(row['copy_bytes'] < row['v5_copy_bytes'] for row in rows),
        copy_unchanged=sum(row['copy_bytes'] == row['v5_copy_bytes'] for row in rows),
        copy_regressed=sum(row['copy_bytes'] > row['v5_copy_bytes'] for row in rows),
        generation_mean_seconds=statistics.mean(row['generation_seconds'] for row in rows),
        generation_min_seconds=min(row['generation_seconds'] for row in rows),
        generation_max_seconds=max(row['generation_seconds'] for row in rows),
        generation_total_seconds=sum(row['generation_seconds'] for row in rows),
        v5_historical_generation_mean_seconds=statistics.mean(row['v5_historical_generation_seconds'] for row in rows),
        evaluation_total_seconds=sum(row['evaluation_seconds'] for row in rows),
        max_observed_peak_working_set_bytes=max(row['observed_peak_working_set_bytes'] for row in rows),
        max_sampled_process_threads=max(row['sampled_max_process_threads'] for row in rows),
        data_movement_bytes={key: sum(row['data_movement_bytes'][key] for row in rows) for key in MOVEMENT_KEYS},
        v5_data_movement_bytes={key: sum(row['v5_data_movement_bytes'][key] for row in rows) for key in MOVEMENT_KEYS})
    summary.update(mean_speedup_increase_percent=change_percent(summary['mean_speedup'], summary['v5_mean_speedup']),
                   cycles_sum_reduction_percent=-change_percent(summary['cycles'], summary['v5_cycles']),
                   copy_sum_reduction_percent=-change_percent(summary['copy_bytes'], summary['v5_copy_bytes']))
    report = dict(
        version='0.5.1', measurement_date='2026-10-30', core_limit=5, requested_budget_seconds=30.0,
        completed=True, row_count=10, paths_relative_to='dag_chain_solver_v5_30s/', config=baseline['config'],
        definitions=dict(
            singlecore='Unmodified official single-core baseline; not this solver with -n 1.',
            speedup='singlecore_cycles / cycles; mean_speedup is the arithmetic mean of the ten ratios.',
            changes='(new / historical_v5 - 1) * 100; negative cycles/COPY change means improvement.',
            generation='One measured wall-clock generation per case, including process start, input, search and output; 50 ms process sampling adds observation overhead.',
            evaluation='Separate wall-clock time of the official Python scorer; not included in generation.',
            cycles='Simulated execution cycles, including evaluator-modeled movement and synchronization.',
            copy='Official scheduled_copy_bytes; MiB = bytes / 1048576; partition deltas retain their sign.',
            history='Original v5 timing is a historical single measurement, not a paired statistical CPU benchmark.',
            repeatability='Time cutoff can change candidate count and selected plan with hardware/load; one timed run does not promise repeat-identical outputs.',
            cores='Five cores are available; the selected schedule may leave some without computation.',
            sums='Sums of separate test cases, not the makespan of one combined workload.',
            retention='All ten new measured plans are reported; no per-case fallback to historical v5 results.',
            limits='Cooperative cutoff reserves output time; a required complete seed can exceed budget, or an exhausted search can finish early.',
            tests='Finite regression evidence, not a proof for every legal input.'),
        source_sha256=dict(measured_summary=sha256(summary_path), v5_baseline_report=sha256(baseline_path),
                           v5_historical_timings=sha256(timings_path), v5_official_summary=sha256(previous_official_path),
                           rules_manifest=sha256(manifest_path), **measured['source_sha256']),
        solver_executable_sha256=measured['solver_executable_sha256'],
        historical_solver_executable_sha256=timings['solver_executable_sha256'],
        official_source_sha256=measured['official_source_sha256'],
        artifacts=dict(measured_summary=relative(summary_path), v5_baseline_report=relative(baseline_path),
                       v5_historical_timings=relative(timings_path), v5_official_summary=relative(previous_official_path),
                       rules_manifest=relative(manifest_path)),
        verification=dict(source_input_plan_result_sha256=True, same_official_evaluator_and_config=True,
                          ten_successful_new_plans=True, historical_plan_score_timing_linked=True),
        tests=dict(ctest=ctests, graph_contracts_passed=26, synthetic_official_passed=64,
                   tested_compilers=['MSVC 19.44', 'MinGW GCC 15.1'],
                   untested_toolchains=['Native Linux', 'Required older GCC version'],
                   quick_budget_seconds=validation['quick_test_budget_seconds'],
                   quick_solver_executable_sha256=validation['solver_executable_sha256'],
                   command_report=relative(validation_path), synthetic_report=relative(synthetic_path)),
        summary=summary, rows=rows)

    lines = [
        '# 五核、約 30 秒搜索實測（v0.5.1）', '',
        '保留原 v5。本次對 10 個用例各量測一次方案生成，再使用未修改的官方 Scene B 評分器評分；全部 10 個新方案均通過。', '',
        f"平均加速比 **{summary['mean_speedup']:.6f}×**（原 v5：{summary['v5_mean_speedup']:.6f}×，變化 {summary['mean_speedup_increase_percent']:+.3f}%）。"
        f"以官方 cycles 比較：改善 {summary['improved']}、相同 {summary['unchanged']}、退步 {summary['regressed']}。", '',
        f"生成耗時平均 **{summary['generation_mean_seconds']:.3f} 秒**，最短 {summary['generation_min_seconds']:.3f} 秒、最長 {summary['generation_max_seconds']:.3f} 秒。"
        f"cycles 合計 {summary['cycles']:,}（原 {summary['v5_cycles']:,}，減少 {summary['cycles_sum_reduction_percent']:+.3f}%）；"
        f"搬運合計 {summary['copy_bytes']:,} bytes（原 {summary['v5_copy_bytes']:,}，減少 {summary['copy_sum_reduction_percent']:+.3f}%）。", '',
        '加速比 = 官方單核基準 cycles ÷ 新方案 cycles；平均為 10 個比值的算術平均。五核是可用上限，方案可以少用。MiB = 1,048,576 bytes。表格為顯示精度，JSON 保存原始數值。', '',
        '| 用例 | 新生成秒 | 原 v5 歷史秒 | 單核基準 cycles | 新 cycles | 加速比 | COPY bytes | COPY MiB | 使用計算核 |',
        '|---|---:|---:|---:|---:|---:|---:|---:|---:|',
    ]
    for row in rows:
        lines.append(f"| {row['case']} | {row['generation_seconds']:.3f} | {row['v5_historical_generation_seconds']:.3f} | {row['singlecore_cycles']:,} | {row['cycles']:,} | {row['speedup']:.6f}× | {row['copy_bytes']:,} | {row['copy_MiB']:.3f} | {row['used_compute_cores']} |")
    lines += ['', '與原 v5 五核結果對比；變化率 = (新 / 舊 − 1) × 100%。cycles、搬運量的負數表示下降。', '',
              '| 用例 | 原 v5 cycles | 原 v5 加速比 | 原 v5 COPY bytes | cycles 變化 | 加速比變化 | 搬運變化 |',
              '|---|---:|---:|---:|---:|---:|---:|']
    for row in rows:
        lines.append(f"| {row['case']} | {row['v5_cycles']:,} | {row['v5_speedup']:.6f}× | {row['v5_copy_bytes']:,} | {row['cycles_change_percent']:+.3f}% | {row['speedup_change_percent']:+.3f}% | {row['copy_change_percent']:+.3f}% |")
    lines += ['', '新方案搬運分類直接取官方 `data_movement_bytes`，單位為 bytes。總量 = 原圖 + 新增，新增 = 分核淨增 + 溢出新增。分核淨增可能為負，保留官方數值。原 v5 同樣的完整分類保存在 JSON。', '',
              '| 用例 | 原圖 COPY | 分核淨增 COPY | 溢出新增 COPY | 全部新增 COPY | 總 COPY |',
              '|---|---:|---:|---:|---:|---:|']
    for row in rows:
        m = row['data_movement_bytes']
        lines.append(f"| {row['case']} | {m['original_graph_copy_bytes']:,} | {m['partition_added_copy_bytes']:,} | {m['spill_added_copy_bytes']:,} | {m['added_copy_bytes']:,} | {m['scheduled_copy_bytes']:,} |")
    lines += ['',
        f"量測中的最大行程 working set 為 {summary['max_observed_peak_working_set_bytes']:,} bytes（{summary['max_observed_peak_working_set_bytes']/MIB:.3f} MiB），取樣觀察到最多 {summary['max_sampled_process_threads']} 條行程執行緒。這包含主執行緒等，不等於搜索 worker 數；50 ms 取樣值也不是作業系統保證的真實峰值。", '',
        '生成秒數是這臺電腦啟動程式、讀取輸入、搜索及輸出方案的實際 wall time；官方 cycles 是模擬執行時間，已計入規則中的搬運和同步。官方 Python 評分器另行執行，其 wall time 不計入生成時間；每點 `evaluation_seconds` 見 JSON。', '',
        '原 v5 耗時取自之前保存的單次量測，並非同時重跑的統計對照。本次每點只量測一次 30 秒版本；時間截斷會隨硬體與負載改變候選數量，所以不宣稱重跑必定逐位元相同。保留完整 v5 種子後繼續搜索，以完整候選作為輸出；若種子本身過慢可能超時，若候選空間用盡也可能提前結束。內部估價保留較優候選不代表官方 cycles 必定改善；本報告沒有逐點退回舊方案。', '',
        '驗證：MSVC 19.44 與 MinGW GCC 15.1 各 7/7 個 CTest 通過，MSVC quick 版的 CLI 契約、26/26 個圖契約及 64/64 次合成圖官方評分通過。quick 版使用 0.5 秒預算；正式 10 點使用 30 秒版。已核對新程式來源、輸入、方案、結果 SHA256，舊方案與舊量測連結，以及新舊官方評分器和 config 相同。原生 Linux 與題目要求的較舊 GCC 尚未實測。這些是有限回歸測試，不是所有合法輸入的正確性證明。', '',
        '原始檔案（路徑相對 `dag_chain_solver_v5_30s/`）：', '',
        '- 本次量測：`runs/five_30s/summary.json`；各點 `runs/five_30s/case_XX/` 內有 `plan_n5.json`、`result.json`、`trace.json`、`log.txt`、stdout/stderr。',
        '- 原 v5 結果快照：`reports/v5_results_baseline.json`；單核原始結果：`reports/singlecore_baseline/case_XX.json`。',
        '- 原 v5 生成耗時：`../dag_chain_solver_v5/runs/final/output_checks_msvc/report.json`；原評分：`../dag_chain_solver_v5/runs/final/official_problem2/summary.json`。',
        '- MSVC/MinGW CTest：`build_msvc/Testing/Temporary/LastTest.log`、`build_mingw/Testing/Temporary/LastTest.log`。',
        '- 契約與合成測試：`runs/validation_30s/msvc_validation_commands.json`、`runs/validation_30s/synthetic_msvc/report.json`。',
        '- 每點的新舊方案、結果、trace 路徑及雜湊，見 `reports/FIVE_CORES_30S.json` 的 `rows[].artifacts` 和各 SHA256 欄位。', '',
        '重新整理報告：`python scripts/summarize_five_30s.py`。此腳本只讀取已保存的量測與評分，遇到缺失、失敗或雜湊不符會停止，不會重新求解或評分。', '',
    ]
    json_path = root / 'reports/FIVE_CORES_30S.json'
    md_path = root / 'reports/FIVE_CORES_30S_zh.md'
    json_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    md_path.write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(dict(json=relative(json_path), markdown=relative(md_path), summary=summary), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
