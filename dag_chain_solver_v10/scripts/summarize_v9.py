"""Verify all 40 v9 results and compare with unchanged v8 0.8.1 runs.

Never filters out regressions. The source and executable hashes of each result
set are checked against current v9 files and the unchanged sibling v8 manifest.
"""
import hashlib
import json
import math
import statistics
import sys
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def change(after, before):
    return 100 * (after / before - 1) if before else None


def verify_row(root, row):
    assert row['status'] == 'ok'
    reference = root / 'references/hackathon_20261030'
    assert sha(reference / 'data' / (row['case'] + '.json')) == row['input_sha256']
    assert sha(root / row['plan']) == row['plan_sha256']
    assert sha(root / row['result']) == row['result_sha256']
    result = read(root / row['result'])
    assert result['makespan'] == row['cycles']
    assert result['num_cores'] == row['cores'] and result['scene'] == 'B'
    assert result['data_movement_bytes'] == row['data_movement_bytes']
    movement = result['data_movement_bytes']
    assert movement['scheduled_copy_bytes'] == sum(movement[k] for k in (
        'original_graph_copy_bytes', 'partition_added_copy_bytes', 'spill_added_copy_bytes'))
    assert row['speedup'] == row['singlecore_cycles'] / row['cycles']
    return read(root / row['plan'])


def summarize(rows):
    result = dict(count=len(rows),
                  mean_speedup=statistics.mean(r['speedup'] for r in rows),
                  old_mean_speedup=statistics.mean(r['old_speedup'] for r in rows),
                  generation_mean_seconds=statistics.mean(r['generation_seconds'] for r in rows),
                  old_generation_mean_seconds=statistics.mean(r['old_generation_seconds'] for r in rows),
                  changed_plans=sum(r['plan_changed'] for r in rows),
                  changed_core_assignments=sum(r['mapping_changed'] for r in rows),
                  communication_selected_cases=sum(r['communication_stats']['selected_new'] for r in rows))
    result['mean_speedup_change_percent'] = change(result['mean_speedup'], result['old_mean_speedup'])
    for field in ('cycles', 'copy_bytes', 'spill_bytes', 'partition_bytes'):
        after, before = sum(r[field] for r in rows), sum(r['old_' + field] for r in rows)
        result[field] = after
        result['old_' + field] = before
        result[field + '_change_percent'] = change(after, before)
        for label, relation in (('improved', lambda a, b: a < b),
                                ('equal', lambda a, b: a == b),
                                ('regressed', lambda a, b: a > b)):
            result[field + '_' + label] = sum(relation(r[field], r['old_' + field]) for r in rows)
    return result


def main():
    root = Path(__file__).resolve().parents[1]
    old_root = root.parent / 'dag_chain_solver_v8'
    old_path, new_path = old_root / 'runs/official_upgrade/summary.json', root / 'runs/official_all/summary.json'
    old, new = read(old_path), read(new_path)
    expected = {(f'case_{i:02}', c) for i in range(10) for c in range(2, 6)}
    assert old['completed'] and new['completed']
    assert len(old['rows']) == len(new['rows']) == 40
    assert {(r['case'], r['cores']) for r in old['rows']} == expected
    assert {(r['case'], r['cores']) for r in new['rows']} == expected
    assert old['config_sha256'] == new['config_sha256']
    assert old['official_source_sha256'] == new['official_source_sha256']
    preserved = read(root / 'reports/v8_preservation_manifest.json')['files']
    for name, digest in preserved.items():
        assert sha(old_root / name) == digest, ('v8 file changed', name)
    assert preserved['runs/official_upgrade/summary.json'] == sha(old_path)
    for name, digest in old['source_sha256'].items():
        assert preserved[name] == digest, ('v8 source provenance mismatch', name)
    assert preserved['build_msvc/bin/npu_sched.exe'] == old['solver_executable_sha256']
    for name, digest in new['source_sha256'].items():
        assert sha(root / name) == digest, ('new source changed', name)
    assert sha(Path(new['solver_path'])) == new['solver_executable_sha256']
    ref = root / 'references/hackathon_20261030'
    assert sha(ref / 'data/config.txt') == new['config_sha256']
    for name, digest in new['official_source_sha256'].items():
        assert sha(ref / name) == digest, name
    previous = {(r['case'], r['cores']): r for r in old['rows']}
    rows = []
    for current in sorted(new['rows'], key=lambda r: (r['cores'], r['case'])):
        before = previous[current['case'], current['cores']]
        old_plan, new_plan = verify_row(old_root, before), verify_row(root, current)
        assert before['input_sha256'] == current['input_sha256']
        assert before['singlecore_cycles'] == current['singlecore_cycles']
        # This stage is entered with the complete v8 incumbent. These checks
        # verify its exact traffic and final acceptance invariants, NOT a
        # guarantee that approximate cycles preserve the official makespan.
        communication = current['generation'].get('communication_stats')
        assert isinstance(communication, dict), ('missing communication diagnostics', current['case'], current['cores'])
        context = current['case'], current['cores']
        assert communication['baseline_trace_valid'], ('invalid baseline trace', context)
        old_movement, new_movement = before['data_movement_bytes'], current['data_movement_bytes']
        before_total = communication['before_route_bytes'] + communication['before_spill_bytes']
        after_total = communication['after_route_bytes'] + communication['after_spill_bytes']
        assert before_total == old_movement['scheduled_copy_bytes'], ('entry differs from v8 total traffic', context)
        assert communication['before_spill_bytes'] == old_movement['spill_added_copy_bytes'], ('entry differs from v8 spill', context)
        assert communication['after_spill_bytes'] == new_movement['spill_added_copy_bytes'], ('final spill differs from official', context)
        assert communication['after_route_bytes'] == (new_movement['original_graph_copy_bytes'] +
                                                       new_movement['partition_added_copy_bytes']), ('final route differs from official', context)
        assert after_total == new_movement['scheduled_copy_bytes'], ('final total differs from official', context)
        assert after_total <= before_total, ('internal traffic guard violated', context)
        assert all(type(communication[k]) in (int, float) and math.isfinite(communication[k])
                   for k in ('before_cycles', 'after_cycles')), ('non-finite internal cycles', context)
        assert communication['after_cycles'] <= communication['before_cycles'], ('internal cycles guard violated', context)
        if not communication['selected_new']:
            assert current['plan_sha256'] == before['plan_sha256'], ('fallback differs from v8 plan', context)
        else:
            assert (communication['after_cycles'], after_total) < (communication['before_cycles'], before_total), ('selected candidate has no strict internal improvement', context)
        def mapping(plan):
            return {v: c for c, stream in enumerate(plan['core_schedules']) for v in stream}
        row = dict(case=current['case'], cores=current['cores'],
                   singlecore_cycles=current['singlecore_cycles'],
                   plan_changed=before['plan_sha256'] != current['plan_sha256'],
                   mapping_changed=mapping(old_plan) != mapping(new_plan),
                   feedback_stats=current['generation'].get('feedback_stats'),
                   communication_stats=current['generation'].get('communication_stats'),
                   input_sha256=current['input_sha256'])
        for prefix, value in (('', current), ('old_', before)):
            movement = value['data_movement_bytes']
            for name in ('cycles', 'speedup', 'plan', 'result', 'plan_sha256', 'result_sha256'):
                row[prefix + name] = ('../dag_chain_solver_v8/' + value[name]
                                      if prefix and name in ('plan', 'result') else value[name])
            row[prefix + 'data_movement_bytes'] = movement
            row[prefix + 'copy_bytes'] = movement['scheduled_copy_bytes']
            row[prefix + 'spill_bytes'] = movement['spill_added_copy_bytes']
            row[prefix + 'partition_bytes'] = movement['partition_added_copy_bytes']
            row[prefix + 'generation_seconds'] = value['generation']['seconds']
        for name in ('cycles', 'copy_bytes', 'spill_bytes', 'partition_bytes'):
            row[name + '_change_percent'] = change(row[name], row['old_' + name])
        rows.append(row)
    report = dict(version='0.9.0', comparison_version='0.8.1', completed=True,
                  source_sha256=new['source_sha256'],
                  solver_executable_sha256=new['solver_executable_sha256'],
                  official_source_sha256=new['official_source_sha256'],
                  config_sha256=new['config_sha256'],
                  old_summary_sha256=sha(old_path), measured_summary_sha256=sha(new_path),
                  v8_preservation_manifest_sha256=sha(root / 'reports/v8_preservation_manifest.json'),
                  summary=summarize(rows),
                  by_cores={str(c): summarize([r for r in rows if r['cores'] == c]) for c in range(2, 6)},
                  rows=rows,
                  verified_invariants=dict(entry_v8_traffic=40, final_official_traffic=40,
                                           nonincreasing_exact_total_bytes=40, nonincreasing_estimated_cycles=40,
                                           fallback_plan_identity=sum(not r['communication_stats']['selected_new'] for r in rows)),
                  notes=['All cycles and traffic use the unmodified official scorer; all 40 results are retained.',
                         'Speedup is official single-core cycles divided by measured multicore cycles.',
                         'Before/after generation times are separate single runs, not a controlled CPU comparison.',
                         'Internal cycle estimates are approximate; official cycle regressions are reported.',
                         'Exact traffic matches the v8 entry and official final result; unchanged fallback plans are byte-identical to v8.',
                         'The sibling v8 folder, source, executable and original benchmark results remain unchanged.'])
    output = root / 'reports/RESULTS_V9.json'
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# v9 0.9.0 與 v8 0.8.1 的官方測試比較', '',
             '加速比 = 官方單核 cycles / 本方案 cycles。40 組均使用未修改的官方評分器；保留改善、持平與退步結果。', '',
             '| 核數 | 原平均加速比 | 新平均加速比 | cycles 合計變化 | 總 COPY 變化 | 溢出變化 | 原／新平均生成秒 |',
             '|---:|---:|---:|---:|---:|---:|---:|']
    for label, s in list(report['by_cores'].items()) + [('全部', report['summary'])]:
        lines.append(f'| {label} | {s["old_mean_speedup"]:.6f}× | {s["mean_speedup"]:.6f}× | '
                     f'{s["cycles_change_percent"]:+.4f}% | {s["copy_bytes_change_percent"]:+.4f}% | '
                     f'{s["spill_bytes_change_percent"]:+.4f}% | '
                     f'{s["old_generation_mean_seconds"]:.3f} / {s["generation_mean_seconds"]:.3f} |')
    for c in range(2, 6):
        lines += ['', f'## {c} 核', '',
                  '| 用例 | 原 cycles | 新 cycles | 原／新加速比 | 原／新總 COPY bytes | 原／新溢出 bytes | 原／新生成秒 |',
                  '|---|---:|---:|---:|---:|---:|---:|']
        for r in rows:
            if r['cores'] != c:
                continue
            lines.append(f'| {r["case"]} | {r["old_cycles"]:,} | {r["cycles"]:,} | '
                         f'{r["old_speedup"]:.6f} / {r["speedup"]:.6f} | '
                         f'{r["old_copy_bytes"]:,} / {r["copy_bytes"]:,} | '
                         f'{r["old_spill_bytes"]:,} / {r["spill_bytes"]:,} | '
                         f'{r["old_generation_seconds"]:.3f} / {r["generation_seconds"]:.3f} |')
    lines += ['', 'v8 評測在 `../dag_chain_solver_v8/runs/official_upgrade/`，v9 評測在 `runs/official_all/`。每組均保留 plan、result、trace 和 log。',
              '搬運分類、核心分配是否改變、回饋與通訊分組搜尋統計、完整數值及雜湊見 [RESULTS_V9.json](RESULTS_V9.json)。',
              '生成時間不含官方評分；原／新時間分別量測，不能視為排除環境波動的 CPU 效率實驗。',
              '內部 cycles 使用近似模型；通過內部非退步篩選不等於官方 cycles 保證。']
    output.with_name('RESULTS_V9_zh.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps({'summary': report['summary'], 'by_cores': report['by_cores']}, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
