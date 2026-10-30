"""Summarize existing, incomplete v11 evidence without running any solver/scorer."""
from datetime import datetime, timezone
import json
from pathlib import Path
import statistics

from run_v11_official import read, sha, verify_baseline, check_generation


def main():
    root = Path(__file__).resolve().parents[1]
    manifest, baseline, old_root = verify_baseline(root)
    old = {(x['case'], x['cores']): x for x in baseline['rows']}
    run_path = root / 'runs/official_greedy_only/summary.json'
    run = read(run_path)
    assert run['variant'] == 'greedy_only' and not run['completed']
    assert len(run['rows']) == 20
    assert {(x['case'], x['cores']) for x in run['rows']} == {
        (f'case_{i:02}', n) for n in (2, 3) for i in range(10)}
    for path, digest in run['source_sha256'].items():
        assert sha(root / path) == digest, ('source changed', path)
    assert sha(Path(run['solver_path'])) == run['solver_executable_sha256']
    rows = []
    for raw in sorted(run['rows'], key=lambda x: (x['cores'], x['case'])):
        assert raw['status'] == 'generated'
        before = old[raw['case'], raw['cores']]
        assert sha(root / raw['plan']) == raw['plan_sha256']
        check_generation(raw['generation'], 'greedy_only', before, raw['plan_sha256'])
        s = raw['generation']['annealing_stats']
        assert s['runs'] == [] and s['annealing_seconds'] == 0 and s['sa_generated'] == 0
        winners = [x for x in s['exact_candidates'] if x['improved']]
        winner = winners[-1] if winners else s['exact_candidates'][0]
        assert winner['cycles'] == s['after_cycles']
        assert winner['route_bytes'] + winner['spill_bytes'] == s['after_bytes']
        assert not winner['error']
        rows.append(dict(case=raw['case'], cores=raw['cores'], status='internal_exact_only',
            singlecore_cycles=before['singlecore_cycles'], old_cycles=before['cycles'],
            cycles=s['after_cycles'], old_speedup=before['speedup'],
            speedup=before['singlecore_cycles'] / s['after_cycles'],
            old_copy_bytes=before['data_movement_bytes']['scheduled_copy_bytes'], copy_bytes=s['after_bytes'],
            old_spill_bytes=before['data_movement_bytes']['spill_added_copy_bytes'], spill_bytes=winner['spill_bytes'],
            old_route_bytes=before['data_movement_bytes']['scheduled_copy_bytes'] - before['data_movement_bytes']['spill_added_copy_bytes'],
            route_bytes=winner['route_bytes'], generation_seconds=raw['generation']['seconds'],
            old_generation_seconds=before['generation']['seconds'], added_stage_seconds=s['total_seconds'],
            annealing_seconds=0, winner_phase=winner['phase'], winner_label=winner['label'],
            assignment_changed=s['assignment_changed'], plan=raw['plan'], plan_sha256=raw['plan_sha256']))

    def aggregate(items):
        result = dict(count=len(items),
            mean_speedup=statistics.mean(x['speedup'] for x in items),
            old_mean_speedup=statistics.mean(x['old_speedup'] for x in items),
            cycles_improved=sum(x['cycles'] < x['old_cycles'] for x in items),
            cycles_equal=sum(x['cycles'] == x['old_cycles'] for x in items),
            cycles_regressed=sum(x['cycles'] > x['old_cycles'] for x in items),
            generation_mean_seconds=statistics.mean(x['generation_seconds'] for x in items),
            generation_min_seconds=min(x['generation_seconds'] for x in items),
            generation_max_seconds=max(x['generation_seconds'] for x in items),
            over_15_seconds=sum(x['generation_seconds'] > 15 for x in items),
            over_20_seconds=sum(x['generation_seconds'] > 20 for x in items),
            added_stage_mean_seconds=statistics.mean(x['added_stage_seconds'] for x in items),
            changed_assignments=sum(x['assignment_changed'] for x in items))
        result['mean_speedup_change_percent'] = 100 * (result['mean_speedup'] / result['old_mean_speedup'] - 1)
        for name in ('cycles', 'copy_bytes', 'spill_bytes', 'route_bytes'):
            result[name] = sum(x[name] for x in items)
            result['old_' + name] = sum(x['old_' + name] for x in items)
            result[name + '_change_percent'] = 100 * (result[name] / result['old_' + name] - 1)
        return result

    smoke_path = root / 'runs/official_baseline/summary.json'
    smoke = read(smoke_path)
    assert smoke['completed'] and len(smoke['rows']) == 4 and smoke['variant'] == 'baseline'
    smoke_rows = []
    for x in smoke['rows']:
        assert x['status'] == 'ok'
        for field in ('plan', 'result'):
            assert sha(root / x[field]) == x[field + '_sha256']
        check_generation(x['generation'], 'baseline', old[x['case'], x['cores']], x['plan_sha256'])
        smoke_rows.append({k: x[k] for k in ('case', 'cores', 'cycles', 'speedup', 'data_movement_bytes', 'plan_sha256')}
                          | {'generation_seconds': x['generation']['seconds']})
    result = dict(version='0.11.0', complete_benchmark=False, generated_at_utc=datetime.now(timezone.utc).isoformat(),
        state='Paused at user request; no new solver or evaluator runs during this summary.',
        tested_variant='greedy_only', actual_sa_public_runs=0, external_official_scored_greedy_points=0,
        score_provenance='C++ internal official-rules timing replay only; awaiting external official confirmation.',
        baseline_comparison='Same 20 case/core pairs of v10.1 official results, not the old all-40 mean.',
        summary=aggregate(rows), by_cores={str(n): aggregate([x for x in rows if x['cores'] == n]) for n in (2, 3)},
        rows=rows, official_baseline_smoke=smoke_rows,
        evidence={str(p.relative_to(root)): sha(p) for p in (run_path, smoke_path, root/'reports/v10_1_baseline_manifest.json')},
        source_sha256=run['source_sha256'], solver_path=run['solver_path'], solver_executable_sha256=run['solver_executable_sha256'])
    out = root / 'reports/PARTIAL_UNLIMITED_V11.json'
    out.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    lines = ['# v11 未限時實驗：已完成部分（暫停快照）', '',
        '使用者要求先整理已有資料，本報告只讀既有檔案，沒有重新生成或評分。未進行 15～20 秒限時修改。', '',
        '**這不是退火版的完整成績。** 已有 20 組 greedy_only（2、3 核各 10 組）生成結果；公開用例的 SA 版本尚未開始，4、5 核貪心亦未開始。這 20 組的 cycles／搬運來自 C++ 內部官方規則精算，尚未經外部官方 Python 評分器確認。另 4 組 baseline 檢查已通過外部官方評分，且方案檔與 v10.1 完全相同。', '',
        '加速比 = 同一官方單核基準 cycles ÷ 當前 cycles；以下均與 v10.1 的相同 20 組比較，不能與舊版全部 40 組平均直接比較。搬運單位為 bytes，時間為完整生成程序的本機實測秒數。', '',
        '| 範圍 | v10.1平均加速比 | 僅貪心平均加速比 | 變化 | cycles改善／持平／退步 | 平均生成秒 | 最慢秒 |',
        '|---|---:|---:|---:|---:|---:|---:|']
    for name, a in [(f'{n}核', result['by_cores'][str(n)]) for n in (2, 3)] + [('合計20組', result['summary'])]:
        lines.append(f"| {name} | {a['old_mean_speedup']:.6f}× | {a['mean_speedup']:.6f}× | {a['mean_speedup_change_percent']:+.4f}% | {a['cycles_improved']}／{a['cycles_equal']}／{a['cycles_regressed']} | {a['generation_mean_seconds']:.3f} | {a['generation_max_seconds']:.3f} |")
    a = result['summary']
    lines += ['', f"20組合計 cycles：{a['old_cycles']:,} → {a['cycles']:,}（{a['cycles_change_percent']:+.4f}%）；總COPY：{a['old_copy_bytes']:,} → {a['copy_bytes']:,}（{a['copy_bytes_change_percent']:+.4f}%）；溢出COPY：{a['old_spill_bytes']:,} → {a['spill_bytes']:,}（{a['spill_bytes_change_percent']:+.4f}%）。",
        '', f"完整生成時間 {a['generation_min_seconds']:.3f}～{a['generation_max_seconds']:.3f} 秒，{a['over_15_seconds']}組超過15秒、{a['over_20_seconds']}組超過20秒。新增貪心／入口精算階段平均 {a['added_stage_mean_seconds']:.3f} 秒，已包含在完整生成時間；SA尚未執行，其0秒不代表實測退火性能。90秒是測試watchdog，不是搜尋目標。", '']
    for n in (2, 3):
        lines += [f'## {n}核（內部精算，外部官方未評分）', '',
            '| 用例 | cycles（v10.1→貪心） | 加速比（舊→新） | 總COPY | ΔCOPY | 溢出COPY | Δ溢出 | 生成秒 | 新增階段秒 | 勝出來源 |',
            '|---|---:|---:|---:|---:|---:|---:|---:|---:|---|']
        for x in rows:
            if x['cores'] != n:
                continue
            lines.append(f"| {x['case']} | {x['old_cycles']:,} → {x['cycles']:,} | {x['old_speedup']:.6f} → {x['speedup']:.6f} | {x['copy_bytes']:,} | {x['copy_bytes']-x['old_copy_bytes']:+,} | {x['spill_bytes']:,} | {x['spill_bytes']-x['old_spill_bytes']:+,} | {x['generation_seconds']:.3f} | {x['added_stage_seconds']:.3f} | {x['winner_phase']} |")
        lines.append('')
    lines += ['## 已完成的4組官方基準檢查', '', '這些是重算v10.1的baseline模式，只用於確認基準一致，不能算作新增貪心或SA的成績。', '',
        '| 用例 | 核數 | 官方cycles | 單核加速比 | 總COPY | 生成秒 |', '|---|---:|---:|---:|---:|---:|']
    for x in smoke_rows:
        lines.append(f"| {x['case']} | {x['cores']} | {x['cycles']:,} | {x['speedup']:.6f} | {x['data_movement_bytes']['scheduled_copy_bytes']:,} | {x['generation_seconds']:.3f} |")
    lines += ['', '## 證據位置', '',
        '- `runs/official_greedy_only/summary.json`：20組生成與完整候選診斷；子目錄保留plan。',
        '- `runs/official_baseline/summary.json`：4組官方基準評分；子目錄保留result、trace、plan。',
        '- `PARTIAL_UNLIMITED_V11.json`：逐點數字、來源hash與部分完成狀態。',
        '- `scripts/summarize_v11_partial.py`：只讀彙整，可在來源未變更時重現。', '',
        '目前程式及參數仍為未限時0.11.0。MSVC/MinGW各19項測試、64組合成DAG官方評測及重跑檢查已完成；公開SA40組與本版完整計時差分尚未執行。尚不能下結論說SA提升多少、需要幾秒，或新版本整體40組平均優於v10.1。', '']
    out.with_name('PARTIAL_UNLIMITED_V11_zh.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps({k: result[k] for k in ('summary', 'by_cores')}, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
