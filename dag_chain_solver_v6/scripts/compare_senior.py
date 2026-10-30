"""Compare two freshly measured official five-core runs without mixing plans."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    runs = [json.loads(q.read_text(encoding='utf-8')) for q in (args.baseline, args.candidate)]
    a, b = runs
    assert all(r['completed'] for r in runs)
    for key in ('core_limit', 'requested_budget_seconds', 'config_sha256', 'official_source_sha256'):
        assert a[key] == b[key], ('incomparable settings', key)
    by_case = [{r['case']: r for r in run['rows']} for run in runs]
    assert set(by_case[0]) == set(by_case[1]) and len(by_case[0]) == 10
    rows = []
    for case in sorted(by_case[0]):
        old, new = (r[case] for r in by_case)
        assert old['status'] == new['status'] == 'ok'
        assert old['input_sha256'] == new['input_sha256']
        assert old['singlecore_cycles'] == new['singlecore_cycles']
        copy = [r['data_movement_bytes']['scheduled_copy_bytes'] for r in (old, new)]
        rows.append(dict(case=case, baseline_cycles=old['cycles'], candidate_cycles=new['cycles'],
                         cycle_reduction_percent=100*(1-new['cycles']/old['cycles']),
                         baseline_copy_bytes=copy[0], candidate_copy_bytes=copy[1],
                         copy_reduction_percent=100*(1-copy[1]/copy[0]) if copy[0] else None,
                         baseline_speedup=old['speedup'], candidate_speedup=new['speedup'],
                         baseline_seconds=old['generation']['seconds'],
                         candidate_seconds=new['generation']['seconds']))
    total_a = sum(r['baseline_cycles'] for r in rows)
    total_b = sum(r['candidate_cycles'] for r in rows)
    copy_a = sum(r['baseline_copy_bytes'] for r in rows)
    copy_b = sum(r['candidate_copy_bytes'] for r in rows)
    result = dict(
        baseline_summary_sha256=hashlib.sha256(args.baseline.read_bytes()).hexdigest(),
        candidate_summary_sha256=hashlib.sha256(args.candidate.read_bytes()).hexdigest(),
        core_limit=a['core_limit'], budget_seconds=a['requested_budget_seconds'],
        wins=sum(r['candidate_cycles'] < r['baseline_cycles']-1e-6 for r in rows),
        ties=sum(abs(r['candidate_cycles']-r['baseline_cycles']) <= 1e-6 for r in rows),
        losses=sum(r['candidate_cycles'] > r['baseline_cycles']+1e-6 for r in rows),
        total_baseline_cycles=total_a, total_candidate_cycles=total_b,
        total_cycle_reduction_percent=100*(1-total_b/total_a),
        total_baseline_copy_bytes=copy_a, total_candidate_copy_bytes=copy_b,
        total_copy_reduction_percent=100*(1-copy_b/copy_a),
        mean_baseline_speedup=sum(r['baseline_speedup'] for r in rows)/len(rows),
        mean_candidate_speedup=sum(r['candidate_speedup'] for r in rows)/len(rows),
        rows=rows,
        limitations='One run per case; wall-clock cutoff and host load affect explored candidates. '
                     'No per-case historical fallback. Sums are across independent cases.')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k != 'rows'}, indent=2))


if __name__ == '__main__':
    main()
