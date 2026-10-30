"""Merge the verified single-core baseline and v4 measurements into a portable handoff."""
import hashlib
import json
from pathlib import Path
import statistics
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    root = Path(__file__).resolve().parents[1]
    reports = root/'reports'
    multi_path = reports/'official_v4_20261030.json'
    baseline_path = reports/'v3_baseline_20261030.json'
    multi = json.loads(multi_path.read_text(encoding='utf-8'))
    baseline = json.loads(baseline_path.read_text(encoding='utf-8'))
    assert multi['config_sha256'] == baseline['config_sha256']
    assert multi['evaluator_source_sha256'] == baseline['evaluator_source_sha256']
    rows = []
    single_times = {}
    for item in baseline['baseline_rows']:
        case = item['case']
        source = reports/'singlecore_baseline'/f'{case}.json'
        assert item['status'] == 'ok' and sha(source) == item['result_sha256']
        raw = json.loads(source.read_text(encoding='utf-8'))
        assert raw['makespan'] == item['makespan']
        traffic = raw['data_movement_bytes']
        single_times[case] = item['makespan']
        rows.append(dict(case=case, cores=1, measurement_type='official_singlecore_baseline',
                         singlecore_cycles=item['makespan'], cycles=item['makespan'], speedup=1.0,
                         copy_bytes=traffic['scheduled_copy_bytes'],
                         added_copy_bytes=traffic['added_copy_bytes'],
                         partition_added_copy_bytes=traffic['partition_added_copy_bytes'],
                         spill_copy_bytes=traffic['spill_added_copy_bytes'],
                         original_graph_copy_bytes=traffic['original_graph_copy_bytes'],
                         source_result=source.relative_to(root).as_posix(), plan=None))
    assert len(single_times) == 10 and len(multi['rows']) == 40 and multi['valid'] == 40
    keys = ['case', 'cores', 'singlecore_cycles', 'cycles', 'speedup', 'copy_bytes',
            'added_copy_bytes', 'partition_added_copy_bytes', 'spill_copy_bytes',
            'original_graph_copy_bytes']
    for item in multi['rows']:
        case, cores = item['case'], item['cores']
        assert cores in range(2, 6) and single_times[case] == item['singlecore_cycles']
        assert abs(item['speedup'] - single_times[case]/item['cycles']) < 1e-12
        row = {key: item[key] for key in keys}
        row.update(measurement_type='v4_scene_b_plan',
                   source_result=f'runs/final/official_problem2/{case}_n{cores}/result.json',
                   plan=f'runs/final/output_checks_msvc/{case}_n{cores}_v4.json')
        rows.append(row)
    rows.sort(key=lambda row: (row['case'], row['cores']))
    indexed = {(row['case'], row['cores']): row for row in rows}
    assert len(indexed) == 50
    means = {str(k): statistics.mean(row['speedup'] for row in rows if row['cores'] == k)
             for k in range(1, 6)}
    output = dict(version='0.4.0', measurement_date=multi['date'], row_count=50,
                  existing_measurements_only=True, paths_relative_to='dag_chain_solver_v4/',
                  definitions=dict(single_core='Supplied official singlecore baseline, not a v4 -n 1 run.',
                                   multicore='v4 Scene B plans, evaluated by the supplied unmodified evaluator.',
                                   cores='Available core limit; empty cores are allowed.',
                                   speedup='singlecore_cycles / cycles',
                                   copy_bytes='Total scheduled COPY bytes, including boundary, cross-core and spill traffic.'),
                  config=dict(L1_bytes=1572864, UB_bytes=131072, DDR_bytes_per_cycle=60,
                              cross_core_copy_delay_cycles=1000, precedence='config.txt overrides outdated docs',
                              sha256=multi['config_sha256']),
                  source_report_sha256={multi_path.name: sha(multi_path), baseline_path.name: sha(baseline_path)},
                  mean_speedup_by_cores=means, rows=rows)
    json_path = reports/'RESULTS_1_TO_5_CORES.json'
    json_path.write_text(json.dumps(output, indent=2, ensure_ascii=False), encoding='utf-8')
    lines = [
        '# v4：1～5 核完整結果與資料位置', '',
        '這份文件整合已保存的測試結果，沒有重新執行求解器或評分器。',
        '**給另一個 Codex：優先讀取同目錄 `RESULTS_1_TO_5_CORES.json`，共 50 筆資料。**', '',
        '1 核是新版原評分器的單核基準；2～5 核是 v4 生成方案的原評分器實測。',
        'v4 的 CLI 目前接受 2～5 核，不能把表中的 1 核解讀為 v4 的 `-n 1` 執行結果。',
        '配置以新版 config 為準：L1 1,572,864 bytes、UB 131,072 bytes、DDR 60 bytes/cycle、跨核等待 1,000 cycles。',
        '加速比＝單核 cycles ÷ 多核 cycles。核心數是可用上限，允許空核心。', '',
        '## 資料來源與接手方式', '',
        '以下路徑都相對於 `dag_chain_solver_v4/`：', '',
        '| 路徑 | 內容 |', '|---|---|',
        '| `reports/RESULTS_1_TO_5_CORES.json` | 整合 1～5 核的 50 筆記錄；含 cycles、加速比、總搬運量及搬運分類。 |',
        '| `reports/official_v4_20261030.json`、`.csv` | 原有 2～5 核的 40 筆 v4 結果、v3 對照與生成時間。 |',
        '| `reports/singlecore_baseline/case_XX.json` | 10 個單核原評分器結果。 |',
        '| `reports/v3_baseline_20261030.json` | 單核基準的來源與雜湊，以及 v3 的 40 組對照結果。 |',
        '| `runs/final/official_problem2/case_XX_nN/` | 多核原始 result.json、trace.json、log.txt、run.json。 |',
        '| `runs/final/output_checks_msvc/case_XX_nN_v4.json` | 對應的提交方案 JSON。 |',
        '| `reports/validation_v4_20261030.json` | 編譯、確定性、契約及資源驗證紀錄。 |',
        '| `reports/rules_manifest.json` | 題目包與原評分器檔案雜湊。 |', '',
        '移至另一臺電腦時，帶上完整 `reports/` 即可讀取這些結果及單核基準。',
        '若需要分析執行時序，另帶上 `runs/final/`。`runs/` 被 git 忽略，不能假設 GitHub 上已有它。',
        '提交源碼壓縮包 `dist/dag_chain_solver_v4.zip` 不含 reports、原評分器或測試記錄。',
        '要重新整合這份文件，可在完整工作目錄執行 `python scripts/export_all_core_results.py`。', '',
    ]
    for key, title in [('speedup', '加速比（倍）'), ('cycles', '執行時間（cycles）'),
                       ('copy_bytes', '總搬運量（bytes）')]:
        lines += [f'## {title}', '', '| 用例 | 1 核 | 2 核 | 3 核 | 4 核 | 5 核 |',
                  '|---|---:|---:|---:|---:|---:|']
        for case in sorted(single_times):
            values = [indexed[case, k][key] for k in range(1, 6)]
            cells = [f'{v:.6f}' if key == 'speedup' else f'{v:,}' for v in values]
            lines.append('| ' + case + ' | ' + ' | '.join(cells) + ' |')
        if key == 'speedup':
            lines.append('| 算術平均 | ' + ' | '.join(f'{means[str(k)]:.6f}' for k in range(1, 6)) + ' |')
        lines.append('')
    lines += ['總搬運量包含圖輸入／輸出、跨核及溢出 COPY；JSON 另列新增與溢出搬運量。',
              '原評分器的負 partition_added_copy_bytes 原樣保留。更多核心不保證實測 cycles 單調下降。', '']
    md_path = reports/'RESULTS_1_TO_5_CORES_zh.md'
    md_path.write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(dict(rows=len(rows), markdown=str(md_path), json=str(json_path)),
                     ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
