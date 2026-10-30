"""Generate deadline-enabled v11.1 plans serially, then score with the official evaluator.

The formal program targets 20 seconds for the whole process; observed overruns
and regressions against v10.1 are retained. The 90 second harness timeout is a
watchdog, never the optimization target. Historical evidence is read-only.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zipfile


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def verify_baseline(root):
    """Verify tested ZIP, unchanged executables/results and recorded EOL-only drift.

    Historical raw source hashes are not silently redefined: the tested ZIP is
    authoritative; current CRLF bytes and their recorded hashes are separate.
    """
    manifest=read(root/'reports/v10_1_baseline_manifest.json')
    base=Path(manifest['baseline_root'])
    archive=base/manifest['tested_archive']['path']
    assert sha(archive)==manifest['tested_archive']['sha256']
    with zipfile.ZipFile(archive) as z:
        for name,digest in manifest['tested_source_sha256'].items():
            tested=z.read(manifest['tested_archive']['source_members'][name])
            assert hashlib.sha256(tested).hexdigest()==digest,('tested ZIP source',name)
            current=(base/name).read_bytes()
            assert hashlib.sha256(current).hexdigest()==manifest['current_source_sha256'][name],('baseline working bytes changed',name)
            assert current.replace(b'\r\n',b'\n')==tested.replace(b'\r\n',b'\n'),('baseline content changed',name)
    for name,digest in manifest['binary_sha256'].items():assert sha(base/name)==digest,('baseline binary changed',name)
    for group in ('evidence_sha256','input_sha256'):
        for name,digest in manifest[group].items():assert sha(base/name)==digest,('baseline evidence changed',name)
    run=read(base/manifest['baseline_run'])
    assert run['completed'] and len(run['rows'])==40 and run['version']=='0.10.1'
    assert run['source_sha256']==manifest['tested_source_sha256']
    return manifest,run,base


def check_generation(timing):
    """Check internal final checkpoint; historical v10.1 may beat a timed run."""
    budget=timing['budget_stats'];stats=timing['annealing_stats']
    assert budget['enabled'] is True and budget['target_seconds']==20 and budget['search_stop_seconds']==18
    assert 0<=budget['elapsed_seconds']<=timing['seconds']+0.01
    assert isinstance(budget['deadline_reached'],bool)
    assert stats['stage']=='annealed' and stats['baseline_valid']
    assert stats['after_cycles'] is not None and stats['after_bytes']>=0
    baseline=(stats['baseline_cycles'],stats['baseline_bytes']);after=(stats['after_cycles'],stats['after_bytes'])
    assert after<=baseline and 1<=stats['exact_evaluations']<=9
    if stats['greedy_cycles'] is not None:
        assert after<=(stats['greedy_cycles'],stats['greedy_bytes'])<=baseline
    expected={'seed':8,'feedback':10,'communication':11.5,'v10':14.5,'annealing':18}
    assert [p['name'] for p in budget['phases']]==list(expected)
    previous=0
    for phase in budget['phases']:
        assert phase['limit_seconds']==expected[phase['name']]
        assert 0<=phase['seconds']<=phase['elapsed_end_seconds']+0.01
        assert previous<=phase['elapsed_end_seconds']<=budget['elapsed_seconds']+0.01
        assert isinstance(phase['deadline_reached'],bool)
        previous=phase['elapsed_end_seconds']


def main():
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tests'))
    from check_outputs import generate
    from check_rules_outputs import check_plan
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--solver', type=Path, default=root/'build_msvc_budget20/bin/npu_sched.exe')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--cores', type=int, nargs='+', default=[2,3,4,5])
    parser.add_argument('--cases', nargs='+', help='Optional case names for development smoke checks')
    parser.add_argument('--points', nargs='+', help='Explicit case:cores pairs instead of the case/core Cartesian product')
    parser.add_argument('--stage', choices=['all', 'generate', 'evaluate'], default='all')
    args = parser.parse_args()
    out = (args.out or root/'runs/budget20_official').resolve()
    assert out not in {(root/'runs'/name).resolve() for name in ('official_baseline','official_greedy_only','official_annealed')}
    out.mkdir(parents=True, exist_ok=True)
    core_limits = sorted(set(args.cores)); assert core_limits and set(core_limits) <= {2,3,4,5}
    solver_extra=[];solver=args.solver.resolve()
    version = subprocess.run([str(solver), '--version'], capture_output=True, check=True, timeout=10)
    assert version.stdout.decode('utf-8').strip() == 'dag-chain-solver 0.11.1'
    reference = root / 'references/hackathon_20261030'
    manifest = read(root / 'reports/rules_manifest.json')
    for name, digest in manifest['files'].items():
        assert sha(reference / name) == digest, ('official file changed', name)
    preservation_path = root / 'reports/v10_1_baseline_manifest.json'
    preservation, baseline, baseline_root = verify_baseline(root)
    old = {(r['case'], r['cores']): r for r in baseline['rows']}
    assert baseline_root != out and baseline_root not in out.parents, 'Do not write into v10 baseline'
    config = reference / 'data/config.txt'
    sources = sorted((root / 'src').glob('*')) + [root / 'CMakeLists.txt']
    metadata = dict(version='0.11.1', variant='budget20_annealed', solver_extra=solver_extra,
                    requested_cases=sorted(args.cases) if args.cases else None, requested_points=sorted(set(args.points)) if args.points else None,
                    core_limits=core_limits, search_mode='whole_process_deadline', baseline_manifest_sha256=sha(preservation_path),
                    baseline_summary_sha256=sha(baseline_root/preservation['baseline_run']),
                    requested_budget_seconds=20, solver_path=str(solver),
                    solver_executable_sha256=sha(solver), config_sha256=sha(config),
                    source_sha256={q.relative_to(root).as_posix(): sha(q) for q in sources if q.is_file()},
                    official_source_sha256={q.relative_to(reference).as_posix(): sha(q)
                                             for q in sorted((reference / 'code').glob('*.py'))})
    saved = out / 'summary.json'
    if saved.exists():
        current = read(saved)
        for key, value in metadata.items():
            assert current[key] == value, ('stale run directory', key)
    else:
        current = dict(**metadata, started_utc=now(), rows=[], completed=False)

    def save():
        temporary = saved.with_suffix('.tmp')
        temporary.write_text(json.dumps(current, indent=2), encoding='utf-8')
        temporary.replace(saved)

    known = {(r['case'], r['cores']): r for r in current['rows']}
    assert len(known) == len(current['rows'])
    graphs = sorted((reference / 'data').glob('case_*.json'))
    if args.cases:
        requested = set(args.cases); graphs = [q for q in graphs if q.stem in requested]
        assert {q.stem for q in graphs} == requested, 'unknown case name'
    expected = {(q.stem, n) for n in core_limits for q in graphs}
    if args.points:
        pairs = set()
        for point in args.points:
            name, number = point.rsplit(':',1); pair = (name,int(number))
            assert pair in expected, ('point outside requested cases/cores', point)
            pairs.add(pair)
        expected = pairs
    assert graphs and expected <= set(old) and set(known) <= expected
    for row in current['rows']:
        assert row['input_sha256'] == sha(reference / 'data' / (row['case'] + '.json'))
        if 'plan_sha256' in row:
            assert row['plan_sha256'] == sha(root / row['plan'])
        if row['status'] == 'ok':
            assert row['result_sha256'] == sha(root / row['result'])

    if args.stage in ('all', 'generate'):
        # Never run two generators concurrently: recorded process wall times
        # should not include contention from this harness's other test cases.
        for cores in core_limits:
            for graph in graphs:
                case = graph.stem; key = case, cores
                if key not in expected or key in known:
                    continue
                folder = out / f'{case}_n{cores}'; folder.mkdir(exist_ok=True)
                plan = folder / 'plan.json'
                row = dict(case=case, cores=cores, input_sha256=sha(graph),
                           plan=os.path.relpath(plan, root).replace('\\', '/'),
                           started_utc=now(), status='generation_error')
                try:
                    timing = generate(solver, graph, cores, plan, observe=True, extra_args=solver_extra)
                    row.update(generation=timing, plan_sha256=sha(plan), external_over_20_seconds=timing['seconds']>20)
                    check_plan(read(graph), read(plan), cores)
                    before = old[key]
                    check_generation(timing)
                    row['status'] = 'generated'
                except Exception as error:
                    row['error'] = repr(error)
                row['generation_finished_utc'] = now()
                current['rows'].append(row); known[key] = row; save()
                print(json.dumps(dict(case=case, cores=cores, status=row['status'],
                                      generation_seconds=row.get('generation', {}).get('seconds'),
                                      error=row.get('error'))), flush=True)
                if row['status'] != 'generated':raise SystemExit('Generation failure retained; stopping before other points')

    if args.stage in ('all', 'evaluate'):
        assert set(known) == expected, 'Finish all requested generation attempts first'
        evaluator = reference / 'code/multicore_cut_evaluate_problem_2.py'
        env = dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
        for row in current['rows']:
            if row['status'] != 'generated':
                continue
            case, cores = row['case'], row['cores']
            graph = reference / 'data' / f'{case}.json'; plan = root / row['plan']; folder = plan.parent
            assert sha(plan) == row['plan_sha256']
            command = [sys.executable, str(evaluator), str(graph), str(plan), '--config', str(config),
                       '-o', str(folder / 'result.json'), '--trace-output', str(folder / 'trace.json'),
                       '--log-output', str(folder / 'log.txt')]
            started = time.perf_counter()
            row.update(evaluation_command=command, status='evaluation_error')
            try:
                with (folder / 'stdout.txt').open('w', encoding='utf-8') as stdout, (folder / 'stderr.txt').open('w', encoding='utf-8') as stderr:
                    result = subprocess.run(command, stdout=stdout, stderr=stderr, env=env, timeout=600)
                row['exit_code'] = result.returncode
                assert result.returncode == 0, (folder / 'stderr.txt').read_text(encoding='utf-8', errors='replace')
                score = read(folder / 'result.json')
                assert score['scene'] == 'B' and score['num_cores'] == cores
                assert score['capacity_bytes'] == {'L1': 1572864, 'UB': 131072}
                assert score['bandwidth_bytes_per_cycle'] == 60 and score['cross_core_copy_delay_cycles'] == 1000
                single = read(root / f'reports/singlecore_baseline/{case}.json')
                assert single['makespan'] == old[case, cores]['singlecore_cycles']
                row['observed_official_cycles'] = score['makespan']
                stats = row['generation']['annealing_stats']
                assert stats['after_cycles'] == score['makespan'], ('exact port cycles differ', case, cores, stats['after_cycles'], score['makespan'])
                assert stats['after_bytes'] == score['data_movement_bytes']['scheduled_copy_bytes'], ('exact traffic differs', case, cores)
                row.update(status='ok', cycles=score['makespan'], speedup=single['makespan'] / score['makespan'],
                           singlecore_cycles=single['makespan'], data_movement_bytes=score['data_movement_bytes'],
                           memory_peak_by_core=score['memory_peak_by_core'], result_sha256=sha(folder / 'result.json'),
                           result=os.path.relpath(folder / 'result.json', root).replace('\\', '/'))
            except Exception as error:
                row['error'] = repr(error)
            row['evaluation_seconds'] = time.perf_counter() - started
            save()
            print(json.dumps({k: row.get(k) for k in ('case', 'cores', 'status', 'cycles', 'error')}), flush=True)
            if row['status'] != 'ok':raise SystemExit('Evaluation failure retained; stopping before other points')
        assert sha(solver) == metadata['solver_executable_sha256'], 'Executable changed during tests'
        for name, digest in metadata['source_sha256'].items():
            assert sha(root / name) == digest, ('Source changed during tests', name)
        for name,digest in preservation['evidence_sha256'].items():assert sha(baseline_root/name)==digest, ('old evidence changed',name)
        current['completed'] = len(current['rows']) == len(expected) and all(r['status'] == 'ok' for r in current['rows'])
        current['finished_utc'] = now(); save()
        assert current['completed'], 'Failures are retained; see summary.json'


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
