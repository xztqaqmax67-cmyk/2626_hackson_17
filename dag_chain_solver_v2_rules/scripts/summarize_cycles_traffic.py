"""Compare saved official results; does not run solvers or evaluators."""
import hashlib
import json
from pathlib import Path


MIB = 1024 * 1024
FIELDS = (
    'original_graph_copy_bytes', 'scheduled_copy_bytes', 'added_copy_bytes',
    'partition_added_copy_bytes', 'spill_added_copy_bytes',
)


def aggregate(rows):
    return {
        'cases': len(rows),
        'makespan_sum_cycles': sum(r['makespan'] for r in rows),
        'data_movement_sum_bytes': {
            key: sum(r['data_movement_bytes'][key] for r in rows) for key in FIELDS
        },
    }


def main():
    root = Path(__file__).resolve().parents[1]
    sources = {
        'v2': root / 'reports/official_comparison_20261030.json',
        'v3': root.parent / 'dag_chain_solver_v3/reports/official_evaluation_v3_1_20261030.json',
    }
    data = {key: json.loads(path.read_text(encoding='utf-8')) for key, path in sources.items()}
    indexes = {key: {(r['case'], r['cores']): r for r in value['rows']} for key, value in data.items()}
    expected = {(f'case_{i:02d}', n) for i in range(10) for n in range(2, 6)}
    assert all(set(index) == expected for index in indexes.values())
    assert all(len(value['rows']) == 40 for value in data.values())
    assert data['v2']['config_sha256'] == data['v3']['config_sha256']
    baseline_rows = data['v3']['baseline_rows']
    assert len(baseline_rows) == 10
    assert {r['case'] for r in baseline_rows} == {f'case_{i:02d}' for i in range(10)}
    for row in [*data['v2']['rows'], *data['v3']['rows'], *baseline_rows]:
        assert row['status'] == 'ok' and row['exit_code'] == 0
        m = row['data_movement_bytes']
        assert m['scheduled_copy_bytes'] == m['original_graph_copy_bytes'] + m['added_copy_bytes']
        assert m['added_copy_bytes'] == m['partition_added_copy_bytes'] + m['spill_added_copy_bytes']
    pairs = []
    for key in sorted(expected):
        a, b = (indexes[version][key] for version in ('v2', 'v3'))
        assert a['input_sha256'] == b['input_sha256']
        assert a['plan_sha256'] == b['plan_sha256']
        assert a['config_sha256'] == b['config_sha256']
        assert a['evaluator_sha256'] == b['evaluator_sha256']
        assert a['makespan'] == b['makespan']
        assert a['data_movement_bytes'] == b['data_movement_bytes']
        pairs.append(dict(case=key[0], cores=key[1],
                          v2_cycles=a['makespan'], v3_cycles=b['makespan'], delta_cycles=0,
                          v2_data_movement_bytes=a['data_movement_bytes'],
                          v3_data_movement_bytes=b['data_movement_bytes'],
                          delta_data_movement_bytes={field: 0 for field in FIELDS}))
    baseline = aggregate(baseline_rows)
    groups = {}
    for n in range(2, 6):
        group = aggregate([r for r in data['v2']['rows'] if r['cores'] == n])
        group['cycles_reduction_vs_baseline_percent'] = 100 * (1 - group['makespan_sum_cycles'] / baseline['makespan_sum_cycles'])
        group['traffic_change_vs_baseline_percent'] = 100 * (group['data_movement_sum_bytes']['scheduled_copy_bytes'] / baseline['data_movement_sum_bytes']['scheduled_copy_bytes'] - 1)
        groups[str(n)] = group
    output = dict(date='2026-10-30', serial_version=data['v2']['version'], parallel_version=data['v3']['version'],
                  source_sha256={key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in sources.items()},
                  aggregation='Sum over the same 10 independent cases, separately for each core limit; v2 and v3 identical.',
                  baseline='Supplied unmodified singlecore_evaluate.py; not a claimed hidden organizer baseline.',
                  comparison_count=40, exact_metric_matches=40, baseline_totals=baseline,
                  by_cores_common_to_v2_and_v3=groups, rows=pairs)
    (root / 'reports/cycles_traffic_comparison_20261030.json').write_text(json.dumps(output, indent=2) + '\n', encoding='utf-8')
    lines = [
        '# 官方 cycles 與搬運量比較', '',
        '日期：2026-10-30。新規則 v2_rules `0.2.1`（單執行緒）對照 v3 `0.3.1`（多執行緒）。', '',
        '**40/40 組的 Makespan 與全部五項搬運量欄位逐項完全相同，差值均為 0。**',
        '本表直接整理已完成的官方評分結果，沒有重新執行求解或評分。原 v2 未修改。', '',
        '## 統計口徑', '',
        '- cycles 是評分器模擬排程的 Makespan，包含計算、搬運及等待造成的完成時間；不是 C++ 求解器的實際執行秒數。',
        '- 每列合計同一組 10 個獨立用例；不是把 10 張圖合併執行的時間，也不是逐例加速比的算術平均。',
        '- N 是允許使用的 NPU 核心數，生成方案可能只使用其中一部分；不是 CPU 搜尋執行緒數。',
        '- 搬運量採官方 `scheduled_copy_bytes`，包含原圖 COPY、切分淨增 COPY 與 spill COPY。1 MiB = 1,048,576 bytes。',
        '- baseline 使用同包原版 `singlecore_evaluate.py` 和相同 config；不是對隱藏評測基準的推測。', '',
        '## 十例合計：v2 和 v3 共用相同數值', '',
        '| 配置 | cycles 合計 | 總搬運量（MiB） | cycles 較 baseline 減少 | 搬運量較 baseline 變化 |',
        '| --- | ---: | ---: | ---: | ---: |',
        f"| 同包單核 baseline | {baseline['makespan_sum_cycles']:,} | {baseline['data_movement_sum_bytes']['scheduled_copy_bytes']/MIB:,.2f} | — | — |",
    ]
    for n, g in groups.items():
        lines.append(f"| v2 = v3，N={n} | {g['makespan_sum_cycles']:,} | {g['data_movement_sum_bytes']['scheduled_copy_bytes']/MIB:,.2f} | {g['cycles_reduction_vs_baseline_percent']:.2f}% | {g['traffic_change_vs_baseline_percent']:+.2f}% |")
    lines += ['', '## 搬運量構成：兩版相同', '',
              '官方欄位關係：`scheduled = original + added`；`added = partition_added + spill_added`。',
              '`partition_added` 是切分後 Task graph COPY 相對原圖 COPY 的淨變化，不能直接解讀為純跨核傳輸量。',
              '此欄位可能為負；例如同包單核 baseline 的 case_08 為 -7,276,544 bytes。本報告保留原值，不截斷為 0。', '',
              '| N | 原圖 COPY（MiB） | 切分淨增 COPY（MiB） | 溢出新增 COPY（MiB） | 新增合計（MiB） | 總 COPY（MiB） |',
              '| --- | ---: | ---: | ---: | ---: | ---: |']
    for n, g in groups.items():
        m = g['data_movement_sum_bytes']
        lines.append('| ' + n + ' | ' + ' | '.join(f'{m[key]/MIB:,.2f}' for key in (FIELDS[0], FIELDS[3], FIELDS[4], FIELDS[2], FIELDS[1])) + ' |')
    lines += ['', '4 核的十例總搬運量最低；5 核的 cycles 合計稍低，但搬運量增加。個別用例未必隨核心數增加而改善。',
              '多執行緒只改變本次生成方案的耗時；既有受控測試整體生成速度為 1.98×，不能將它當作 NPU cycles 的改善。', '',
              '## 逐例比較', '',
              '以下各列 cycles 與搬運量均同時適用 v2、v3；兩版差值為 0。MiB 四捨五入至小數點後兩位；JSON 保存精確 bytes。', '',
              '| 用例 | N | cycles（v2 = v3） | 總 COPY（MiB） | 切分淨增（MiB） | 溢出新增（MiB） |',
              '| --- | --- | ---: | ---: | ---: | ---: |']
    for row in pairs:
        m = row['v2_data_movement_bytes']
        lines.append(f"| {row['case']} | {row['cores']} | {row['v2_cycles']:,} | {m[FIELDS[1]]/MIB:,.2f} | {m[FIELDS[3]]/MIB:,.2f} | {m[FIELDS[4]]/MIB:,.2f} |")
    lines += ['', '## 來源與重現', '',
              '- [v2 官方結果](reports/official_comparison_20261030.json)',
              '- [v3 官方結果及單核 baseline](../dag_chain_solver_v3/reports/official_evaluation_v3_1_20261030.json)',
              '- [本表精確數值、逐項差值及來源 SHA256](reports/cycles_traffic_comparison_20261030.json)',
              '- [求解耗時比較](THREADING_COMPARISON.md)',
              '- 執行 `python scripts/summarize_cycles_traffic.py` 可重新整理本表；只讀取既有結果，不觸發評分。', '']
    (root / 'CYCLES_AND_TRAFFIC_COMPARISON.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(dict(exact_metric_matches=40, baseline=baseline, by_cores=groups), indent=2))


if __name__ == '__main__':
    main()
