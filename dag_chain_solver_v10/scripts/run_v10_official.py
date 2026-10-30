"""Generate all v10 plans for one cumulative variant serially, then score using the unmodified official code.

Default search is fixed-work; no 15/30 second target is imposed by this harness.
The 90 second generator timeout in tests/check_outputs.py is a test watchdog.
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


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def now():
    return datetime.now(timezone.utc).isoformat()


def main():
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tests'))
    from check_outputs import generate
    from check_rules_outputs import check_plan
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--solver', type=Path, help='Default: formal solver for exact, experiment solver for other variants')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--variant', choices=['v9','tensor_memory','multi_feedback','exact'], default='exact')
    parser.add_argument('--cores', type=int, nargs='+', default=[2,3,4,5])
    parser.add_argument('--cases', nargs='+', help='Optional case names for development smoke checks')
    parser.add_argument('--solver-extra', nargs=argparse.REMAINDER, default=[])
    parser.add_argument('--stage', choices=['all', 'generate', 'evaluate'], default='all')
    args = parser.parse_args()
    out = (args.out or root / ('runs/official_' + args.variant)).resolve(); out.mkdir(parents=True, exist_ok=True)
    core_limits = sorted(set(args.cores)); assert core_limits and set(core_limits) <= {2,3,4,5}
    solver_extra = args.solver_extra or (['--ablation', args.variant] if args.variant != 'exact' else [])
    if '--ablation' in solver_extra:
        position = solver_extra.index('--ablation')
        assert position + 1 < len(solver_extra) and solver_extra[position+1] == args.variant, 'variant/ablation mismatch'
    for option in solver_extra:
        if option.startswith('--ablation='):
            assert option.split('=',1)[1] == args.variant, 'variant/ablation mismatch'
    solver = (args.solver or root / ('build_msvc/bin/npu_sched.exe' if args.variant == 'exact' else 'build_msvc/bin/npu_sched_experiment.exe')).resolve()
    limits = (root / 'src/search_limits.hpp').read_text(encoding='utf-8')
    assert re.search(r'inline\s+constexpr\s+bool\s+enable_timed_search\s*=\s*false\s*;', limits), 'This report requires default fixed-work search'
    version = subprocess.run([str(solver), '--version'], capture_output=True, check=True, timeout=10)
    assert version.stdout.decode('utf-8').strip() == 'dag-chain-solver 0.10.0'
    reference = root / 'references/hackathon_20261030'
    manifest = read(root / 'reports/rules_manifest.json')
    for name, digest in manifest['files'].items():
        assert sha(reference / name) == digest, ('official file changed', name)
    baseline = read(root / 'reports/v5_results_baseline.json')
    old = {(r['case'], r['cores']): r for r in baseline['rows'] if r['cores'] in (2, 3, 4, 5)}
    config = reference / 'data/config.txt'
    sources = sorted((root / 'src').glob('*')) + [root / 'CMakeLists.txt']
    metadata = dict(version='0.10.0', variant=args.variant, solver_extra=solver_extra,
                    requested_cases=sorted(args.cases) if args.cases else None, core_limits=core_limits, search_mode='fixed_work',
                    requested_budget_seconds=None, solver_path=str(solver),
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
                if key in known:
                    continue
                folder = out / f'{case}_n{cores}'; folder.mkdir(exist_ok=True)
                plan = folder / 'plan.json'
                row = dict(case=case, cores=cores, input_sha256=sha(graph),
                           plan=os.path.relpath(plan, root).replace('\\', '/'),
                           started_utc=now(), status='generation_error')
                try:
                    timing = generate(solver, graph, cores, plan, observe=True, extra_args=solver_extra)
                    assert timing['v10_stats']['stage'] == args.variant, 'runtime stage does not match requested variant'
                    check_plan(read(graph), read(plan), cores)
                    row.update(generation=timing, plan_sha256=sha(plan), status='generated')
                except Exception as error:
                    row['error'] = repr(error)
                row['generation_finished_utc'] = now()
                current['rows'].append(row); known[key] = row; save()
                print(json.dumps(dict(case=case, cores=cores, status=row['status'],
                                      generation_seconds=row.get('generation', {}).get('seconds'),
                                      error=row.get('error'))), flush=True)

    if args.stage in ('all', 'evaluate'):
        assert set(known) == expected, 'Finish all 40 generation attempts first'
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
                row.update(status='ok', cycles=score['makespan'], speedup=single['makespan'] / score['makespan'],
                           singlecore_cycles=single['makespan'], data_movement_bytes=score['data_movement_bytes'],
                           memory_peak_by_core=score['memory_peak_by_core'], result_sha256=sha(folder / 'result.json'),
                           result=os.path.relpath(folder / 'result.json', root).replace('\\', '/'))
            except Exception as error:
                row['error'] = repr(error)
            row['evaluation_seconds'] = time.perf_counter() - started
            save()
            print(json.dumps({k: row.get(k) for k in ('case', 'cores', 'status', 'cycles', 'error')}), flush=True)
        assert sha(solver) == metadata['solver_executable_sha256'], 'Executable changed during tests'
        for name, digest in metadata['source_sha256'].items():
            assert sha(root / name) == digest, ('Source changed during tests', name)
        current['completed'] = len(current['rows']) == len(expected) and all(r['status'] == 'ok' for r in current['rows'])
        current['finished_utc'] = now(); save()
        assert current['completed'], 'Failures are retained; see summary.json'


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
