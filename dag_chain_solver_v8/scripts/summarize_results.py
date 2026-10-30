"""Compare saved v5 official scores with the pinned v4 50-row report; no scoring."""
import hashlib
import json
from pathlib import Path
import statistics


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    reports = root / 'reports'
    baseline_path = reports / 'v4_results_1_to_5_baseline.json'
    baseline = json.loads(baseline_path.read_text(encoding='utf-8'))
    old = {(r['case'], r['cores']): r for r in baseline['rows']}
    expected = {(f'case_{i:02d}', k) for i in range(10) for k in range(1, 6)}
    assert set(old) == expected and len(baseline['rows']) == 50
    official_path = root / 'runs/final/official_problem2/summary.json'
    official = json.loads(official_path.read_text(encoding='utf-8'))
    generation = json.loads((root / 'runs/final/output_checks_msvc/report.json').read_text(encoding='utf-8'))
    assert official['version'] == generation['version'] == '0.5.0'
    assert official['generator_executable_sha256'] == generation['solver_executable_sha256']
    assert official['config_sha256'] == baseline['config']['sha256']
    newer = {(r['case'], r['cores']): r for r in official['rows']}
    assert set(newer) == {x for x in expected if x[1] > 1} and len(official['rows']) == 40
    assert all(r['status'] == 'ok' for r in newer.values()), 'Failed scores must not be silently excluded'
    rows = []
    for key in sorted(expected):
        b = old[key]
        original_result = root.parent / 'dag_chain_solver_v4' / b['source_result']
        raw_old = json.loads(original_result.read_text(encoding='utf-8'))
        assert raw_old['makespan'] == b['cycles']
        assert raw_old['data_movement_bytes']['scheduled_copy_bytes'] == b['copy_bytes']
        if key[1] == 1:
            row = dict(b)
            assert digest(reports / 'singlecore_baseline' / f'{key[0]}.json') == digest(original_result)
        else:
            r = newer[key]
            m = r['data_movement_bytes']
            assert m['scheduled_copy_bytes'] == m['original_graph_copy_bytes'] + m['added_copy_bytes']
            assert m['added_copy_bytes'] == m['partition_added_copy_bytes'] + m['spill_added_copy_bytes']
            row = dict(case=key[0], cores=key[1], measurement_type='v5_scene_b_plan',
                       singlecore_cycles=b['singlecore_cycles'], cycles=r['makespan'],
                       speedup=b['singlecore_cycles']/r['makespan'],
                       copy_bytes=m['scheduled_copy_bytes'], added_copy_bytes=m['added_copy_bytes'],
                       partition_added_copy_bytes=m['partition_added_copy_bytes'],
                       spill_copy_bytes=m['spill_added_copy_bytes'], original_graph_copy_bytes=m['original_graph_copy_bytes'],
                       source_result=f'runs/final/official_problem2/{key[0]}_n{key[1]}/result.json',
                       plan=f'runs/final/output_checks_msvc/{key[0]}_n{key[1]}_v5.json')
        row.update(v4_cycles=b['cycles'], v4_copy_bytes=b['copy_bytes'],
                   cycles_change_percent=100*(row['cycles']/b['cycles']-1),
                   copy_bytes_change=row['copy_bytes']-b['copy_bytes'])
        rows.append(row)
    by_cores = {}
    for k in range(1, 6):
        subset = [r for r in rows if r['cores'] == k]
        by_cores[str(k)] = dict(
            v4_mean_speedup=statistics.mean(old[r['case'], k]['speedup'] for r in subset),
            v5_mean_speedup=statistics.mean(r['speedup'] for r in subset),
            **{name: sum(r[name] for r in subset) for name in
               ('v4_cycles','cycles','v4_copy_bytes','copy_bytes','original_graph_copy_bytes',
                'partition_added_copy_bytes','spill_copy_bytes')})
    multi = [r for r in rows if r['cores'] > 1]
    audit_path = reports/'extension_audit.json'
    if audit_path.exists():
        audit = json.loads(audit_path.read_text(encoding='utf-8'))
        assert audit['completed']
        labels = {(r['case'],r['cores']):r['v5']['label'] for r in audit['rows']}
        for r in rows:
            if (r['case'],r['cores']) in labels:
                r['selected_candidate_label'] = labels[r['case'],r['cores']]
    summary = dict(
        v4_mean_speedup=statistics.mean(old[r['case'], r['cores']]['speedup'] for r in multi),
        v5_mean_speedup=statistics.mean(r['speedup'] for r in multi),
        improved=sum(r['cycles'] < r['v4_cycles'] for r in multi),
        unchanged=sum(r['cycles'] == r['v4_cycles'] for r in multi),
        regressed=sum(r['cycles'] > r['v4_cycles'] for r in multi),
        **{name:sum(r[name] for r in multi) for name in ('v4_cycles','cycles','v4_copy_bytes','copy_bytes')})
    summary['mean_speedup_increase_percent'] = 100*(summary['v5_mean_speedup']/summary['v4_mean_speedup']-1)
    summary['cycles_sum_reduction_percent'] = 100*(1-summary['cycles']/summary['v4_cycles'])
    summary['copy_sum_reduction_percent'] = 100*(1-summary['copy_bytes']/summary['v4_copy_bytes'])
    artifact = dict(version='0.5.0', measurement_date='2026-10-30', row_count=50,
                    paths_relative_to='dag_chain_solver_v5/', config=baseline['config'],
                    definitions=dict(single_core='Official singlecore baseline, not v5 -n 1.',
                                     multicore='v5 plans scored by unmodified official problem-2 evaluator.',
                                     means='Arithmetic mean of 40 per-run singlecore/multicore ratios.',
                                     sums='Sum of independent runs; not a combined workload.'),
                    source_sha256=dict(v4_report=digest(baseline_path), v5_official_summary=digest(official_path)),
                    summary=summary, by_cores=by_cores, rows=rows)
    (reports/'RESULTS_1_TO_5_CORES.json').write_text(json.dumps(artifact,indent=2),encoding='utf-8')
    (reports/'official_v5_20261030.json').write_text(json.dumps(official,indent=2),encoding='utf-8')
    lines = ['# v4 / v5 官方結果比較', '',
             '1 核為同包原評分器的單核基準，並非 v4/v5 的 `-n 1`；2～5 核為各版實測。',
             '輸入、config 與官方評分器相同；v4 沿用使用者提供的既有結果，v5 重新生成並評分。',
             '求解器不讀報表、不按公開用例或答案分支。公開測試不能代替任意輸入的合法性論證。', '',
             f"40 組平均排程加速比：v4 **{summary['v4_mean_speedup']:.6f}×**；v5 **{summary['v5_mean_speedup']:.6f}×**。",
             f"cycles 降低 {summary['improved']} 組、持平 {summary['unchanged']} 組、增加 {summary['regressed']} 組。", '',
             '## 十例合計與平均加速比', '',
             'cycles / bytes 是十例合計；平均加速比是逐例比值的算術平均。核心數為可用上限。', '',
             '| 核數 | v4 平均加速比 | v5 平均加速比 | v4 cycles | v5 cycles | v4 COPY MiB | v5 COPY MiB |',
             '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for k,g in by_cores.items():
        lines.append(f"| {k} | {g['v4_mean_speedup']:.6f} | {g['v5_mean_speedup']:.6f} | {g['v4_cycles']:,} | {g['cycles']:,} | {g['v4_copy_bytes']/1048576:,.2f} | {g['copy_bytes']/1048576:,.2f} |")
    lines += ['', '## v5 搬運量構成', '',
              '1 MiB = 1,048,576 bytes。原圖＋切分淨新增＋spill＝總 COPY；負切分淨新增保留原值。', '',
              '| 核數 | 原圖 MiB | 切分淨新增 MiB | spill MiB | 總 COPY MiB |', '| --- | ---: | ---: | ---: | ---: |']
    for k,g in by_cores.items():
        lines.append('| '+k+' | '+' | '.join(f'{g[f]/1048576:,.2f}' for f in ('original_graph_copy_bytes','partition_added_copy_bytes','spill_copy_bytes','copy_bytes'))+' |')
    lines += ['', '## 逐例結果', '',
              '| 用例 | 核數 | v4 cycles | v5 cycles | cycles 變化 | v5 單核加速比 | v4 COPY bytes | v5 COPY bytes |',
              '| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    for r in rows:
        lines.append(f"| {r['case']} | {r['cores']} | {r['v4_cycles']:,} | {r['cycles']:,} | {r['cycles_change_percent']:+.3f}% | {r['speedup']:.6f} | {r['v4_copy_bytes']:,} | {r['copy_bytes']:,} |")
    lines += ['', '## 變更方案的來源', '',
              '以下標籤來自測試診斷程式，與正式輸出核對；停用新增策略可重現對應 v4 方案（只正規化 CRLF/LF）。', '',
              '| 用例 | 核數 | v5 勝出候選 | cycles 變化 | COPY bytes 變化 |',
              '| --- | --- | --- | ---: | ---: |']
    for r in multi:
        if r['cycles'] != r['v4_cycles'] or r['copy_bytes'] != r['v4_copy_bytes']:
            lines.append(f"| {r['case']} | {r['cores']} | {r.get('selected_candidate_label','尚未核對')} | {r['cycles_change_percent']:+.3f}% | {r['copy_bytes_change']:+,} |")
    lines += ['', '局部 cycles 改善不代表每組搬運量都下降；本表完整保留增加與減少。', '',
              '## 原始檔案', '',
              '- [50 筆精確數值及逐筆檔案位置](RESULTS_1_TO_5_CORES.json)',
              '- [40 組官方結果及來源雜湊](official_v5_20261030.json)',
              '- `../runs/final/official_problem2/case_XX_nN/`：result.json、trace.json、log.txt、run.json。',
              '- `../runs/final/output_checks_msvc/case_XX_nN_v5.json`：提交方案。',
              '- `singlecore_baseline/case_XX.json`：未變動的單核基準。',
              '- `v4_results_1_to_5_baseline.json`：使用者 v4 報表原樣副本。', '',
              '內部估價可能與官方 COPY 排序、容量重用不同；保留 v4 候選不保證官方 cycles 必然不退步。', '']
    (reports/'RESULTS_1_TO_5_CORES_zh.md').write_text('\n'.join(lines),encoding='utf-8')
    (root/'V4_V5_COMPARISON.md').write_text('# v4 / v5 比較\n\n完整結果見 [50 筆比較表](reports/RESULTS_1_TO_5_CORES_zh.md)。\n',encoding='utf-8')
    print(json.dumps(summary,indent=2))


if __name__ == '__main__':
    main()
