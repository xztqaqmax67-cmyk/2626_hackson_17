"""Verify provenance and summarize official Scene B results, never internal estimates."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--official', type=Path, default=root/'runs/final/official_problem2')
    p.add_argument('--generation', type=Path, default=root/'runs/final/output_checks_msvc')
    p.add_argument('--out', type=Path, default=root/'reports')
    a = p.parse_args()
    official = read(a.official/'summary.json')
    generation = read(a.generation/'report.json')
    previous = read(root/'reports/v3_baseline_20261030.json')
    rules = read(root/'reports/rules_manifest.json')
    assert len(official['rows']) == len(generation['rows']) == 40
    assert official['config_sha256'] == previous['config_sha256'] == rules['files']['data/config.txt']
    assert official['evaluator_source_sha256'] == previous['evaluator_source_sha256']
    assert official['generator_executable_sha256'] == generation['solver_executable_sha256']
    baseline = {r['case']: r for r in previous['baseline_rows']}
    for case, row in baseline.items():
        path = root/'reports/singlecore_baseline'/f'{case}.json'
        assert sha(path) == row['result_sha256']
        assert row['config_sha256'] == official['config_sha256']
        assert row['evaluator_sha256'] == official['evaluator_source_sha256']['code/singlecore_evaluate.py']
        assert read(path)['makespan'] == row['makespan']
    old = {(r['case'], r['cores']): r for r in previous['rows']}
    measured = {(r['case'], r['cores']): r for r in generation['rows']}
    rows = []
    for r in official['rows']:
        case, cores = r['case'], r['cores']
        gen, prior, base = measured[case, cores], old[case, cores], baseline[case]
        assert r['status'] == 'ok', r
        assert r['plan_sha256'] == gen['plan_sha256']
        assert sha(a.generation/f'{case}_n{cores}_v4.json') == r['plan_sha256']
        assert sha(a.official/r['artifacts']/'result.json') == r['result_sha256']
        assert r['input_sha256'] == prior['input_sha256'] == base['input_sha256']
        assert gen['repeat_identical'] and gen['structure_passed']
        traffic = r['data_movement_bytes']
        row = dict(case=case, cores=cores, singlecore_cycles=base['makespan'],
                   cycles=r['makespan'], speedup=base['makespan']/r['makespan'],
                   copy_bytes=traffic['scheduled_copy_bytes'],
                   added_copy_bytes=traffic['added_copy_bytes'],
                   partition_added_copy_bytes=traffic['partition_added_copy_bytes'],
                   spill_copy_bytes=traffic['spill_added_copy_bytes'],
                   original_graph_copy_bytes=traffic['original_graph_copy_bytes'],
                   generation_seconds=gen['generation']['seconds'],
                   v3_cycles=prior['makespan'], v3_speedup=prior['speedup'],
                   v3_copy_bytes=prior['data_movement_bytes']['scheduled_copy_bytes'],
                   cycles_reduction_percent=100*(1-r['makespan']/prior['makespan']),
                   repeat_identical=True)
        rows.append(row)
    rows.sort(key=lambda r: (r['case'], r['cores']))
    report = dict(version='0.4.0', date='2026-10-30', official_simulation_run=True,
                  final_organizer_score_claimed=False, valid=len(rows), invalid=0,
                  speedup_formula='supplied singlecore_evaluate.py makespan / Scene B makespan',
                  baseline_reused_after_hash_verification=True,
                  mean_speedup=statistics.mean(r['speedup'] for r in rows),
                  v3_mean_speedup=previous['mean_speedup'],
                  faster=sum(r['cycles'] < r['v3_cycles'] for r in rows),
                  tied=sum(r['cycles'] == r['v3_cycles'] for r in rows),
                  slower=sum(r['cycles'] > r['v3_cycles'] for r in rows),
                  total_copy_bytes=sum(r['copy_bytes'] for r in rows),
                  v3_total_copy_bytes=sum(r['v3_copy_bytes'] for r in rows),
                  maximum_generation_seconds=max(r['generation_seconds'] for r in rows),
                  mean_generation_seconds=statistics.mean(r['generation_seconds'] for r in rows),
                  maximum_sampled_process_threads=max(r['generation']['sampled_max_process_threads'] for r in generation['rows']),
                  maximum_observed_working_set_bytes=max(r['generation']['observed_peak_working_set_bytes'] for r in generation['rows']),
                  config_sha256=official['config_sha256'],
                  evaluator_source_sha256=official['evaluator_source_sha256'],
                  solver_executable_sha256=generation['solver_executable_sha256'],
                  solver_source_sha256={f'src/{p.name}': sha(p) for p in sorted((root/'src').glob('*')) if p.is_file()},
                  by_cores={}, rows=rows)
    for k in range(2, 6):
        selected = [r for r in rows if r['cores'] == k]
        report['by_cores'][str(k)] = dict(mean_speedup=statistics.mean(r['speedup'] for r in selected),
                                        v3_mean_speedup=statistics.mean(r['v3_speedup'] for r in selected),
                                        mean_cycles=statistics.mean(r['cycles'] for r in selected),
                                        mean_copy_bytes=statistics.mean(r['copy_bytes'] for r in selected))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out/'official_v4_20261030.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    with (a.out/'official_v4_20261030.csv').open('w', encoding='utf-8-sig', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    lines = [
        '# v4 原評分器測試結果', '',
        '日期：2026-10-30。10 個用例 × 2、3、4、5 核，共 40 組全部通過。', '',
        '使用 `hackathon.zip` 中未修改的 `multicore_cut_evaluate_problem_2.py`；',
        'L1＝1,572,864 bytes，UB＝131,072 bytes，DDR＝60 bytes/cycle，同步＝1,000 cycles。',
        '加速比＝同一附件的單核評分器 cycles ÷ 多核 cycles。單核基準沿用 v3 的已核對結果；',
        '已驗證輸入、config、全部原評分器程式及單核結果的 SHA-256。這不是主辦方最終排行榜分數。', '',
        f"40 組加速比算術平均：**{report['mean_speedup']:.6f}×**；v3 為 {report['v3_mean_speedup']:.6f}×。",
        f"相對 v3：{report['faster']} 組更快、{report['tied']} 組持平、{report['slower']} 組較慢。",
        f"40 組總搬運量合計：{report['total_copy_bytes']:,} bytes；v3 為 {report['v3_total_copy_bytes']:,} bytes。", '',
        '「搬運量」指 scheduled_copy_bytes，包含圖輸入／輸出、跨核及換出／重載；不是只有新增跨核搬運。',
        'CSV 另列 added、partition_added、spill 三項。原評分器的負 partition_added 值原樣保留。', '',
        '| 核數 | v3 平均加速比 | v4 平均加速比 |', '|---:|---:|---:|']
    for k, value in report['by_cores'].items():
        lines.append(f"| {k} | {value['v3_mean_speedup']:.6f}× | {value['mean_speedup']:.6f}× |")
    lines += ['', '| 用例 | 核數 | 單核 cycles | v4 cycles | 加速比 | 總搬運 bytes | 溢出搬運 bytes | v3 cycles | cycles 降低 |',
              '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
    for r in rows:
        lines.append(f"| {r['case']} | {r['cores']} | {r['singlecore_cycles']:,} | {r['cycles']:,} | "
                     f"{r['speedup']:.6f}× | {r['copy_bytes']:,} | {r['spill_copy_bytes']:,} | "
                     f"{r['v3_cycles']:,} | {r['cycles_reduction_percent']:+.3f}% |")
    lines += ['', f"生成時間平均 {report['mean_generation_seconds']:.3f} 秒，最大 {report['maximum_generation_seconds']:.3f} 秒；",
              f"Windows 取樣觀測最大執行緒 {report['maximum_sampled_process_threads']}，",
              f"最大 working set {report['maximum_observed_working_set_bytes']/1048576:.2f} MiB。",
              '每組重新生成兩次，輸出位元組完全一致；資源數值是本機實測，非所有未知輸入的上界。', '',
              '原評分器逐組 result.json、trace.json、log.txt 與命令記錄：`runs/final/official_problem2/`。',
              '逐組方案與重複生成記錄：`runs/final/output_checks_msvc/`。',
              'v3 原始程式未修改，雜湊記錄在 `v3_source_manifest.json`。', '']
    (a.out/'RESULTS_zh.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ['valid', 'mean_speedup', 'v3_mean_speedup', 'faster', 'tied', 'slower',
                                           'total_copy_bytes', 'v3_total_copy_bytes', 'maximum_generation_seconds',
                                           'maximum_sampled_process_threads', 'maximum_observed_working_set_bytes']}, indent=2))
    for r in rows:
        if r['cores'] == 4:
            print(r['case'], r['cycles'], f"{r['speedup']:.4f}x", r['copy_bytes'], f"{r['copy_bytes']/1048576:.3f} MiB",
                  f"delta={r['cycles_reduction_percent']:+.3f}%")


if __name__ == '__main__':
    main()
