"""Run the supplied, unmodified problem-2 CLI on recorded v0.5.0 plans."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, default=root/'references/hackathon_20261030')
    parser.add_argument('--plans', type=Path, default=root/'runs/final/output_checks_msvc')
    parser.add_argument('--out', type=Path, default=root/'runs/final/official_problem2')
    parser.add_argument('--workers', type=int, choices=range(1, 5), default=2)
    parser.add_argument('--timeout', type=float, default=600,
                        help='Evaluator-process timeout; separate from solver generation limit')
    args = parser.parse_args()
    reference, plans, out = args.reference.resolve(), args.plans.resolve(), args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source_hashes = {p.relative_to(reference).as_posix(): sha(p)
                     for p in sorted((reference/'code').glob('*.py'))}
    settings = reference/'data/config.txt'
    config_hash = sha(settings)
    generation = json.loads((plans/'report.json').read_text(encoding='utf-8'))
    validation = json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
    for name, digest in source_hashes.items():
        assert digest == validation['files'][name], ('official source changed', name)
    assert config_hash == validation['files']['data/config.txt']
    assert generation['version'] == '0.5.0'
    rows = generation['rows']
    assert rows
    env = os.environ.copy()
    env['PYTHONUTF8'] = '1'
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    evaluator = reference/'code/multicore_cut_evaluate_problem_2.py'
    report = dict(version='0.5.0', scoring_performed=True, scene='B',
                  evaluator=str(evaluator), evaluator_source_sha256=source_hashes,
                  config_sha256=config_hash, generator_executable_sha256=generation['solver_executable_sha256'],
                  evaluator_timeout_seconds=args.timeout, evaluator_workers=args.workers, rows=[])

    def evaluate(row):
        case, cores = row['case'], row['cores']
        graph = reference/'data'/f'{case}.json'
        plan = plans/f'{case}_n{cores}_v5.json'
        assert sha(graph) == row['input_sha256']
        assert sha(plan) == row['plan_sha256']
        folder = out/f'{case}_n{cores}'
        folder.mkdir(exist_ok=True)
        identity = dict(input_sha256=sha(graph), plan_sha256=sha(plan),
                        config_sha256=config_hash, evaluator_sha256=sha(evaluator))
        meta_path = folder/'run.json'
        meta = json.loads(meta_path.read_text(encoding='utf-8')) if meta_path.exists() else None
        # Resume the already completed pilot. All official sources were checked
        # against the same recorded package before starting this batch.
        if meta:
            assert all(meta.get(k) == v for k, v in identity.items()), ('stale output', folder)
        else:
            command = [sys.executable, str(evaluator), str(graph), str(plan), '--config', str(settings),
                       '-o', str(folder/'result.json'), '--trace-output', str(folder/'trace.json'),
                       '--log-output', str(folder/'log.txt')]
            start = time.perf_counter()
            with (folder/'stdout.txt').open('w', encoding='utf-8') as stdout, \
                    (folder/'stderr.txt').open('w', encoding='utf-8') as stderr:
                try:
                    process = subprocess.run(command, stdout=stdout, stderr=stderr,
                                             env=env, timeout=args.timeout)
                    status, code = ('ok' if process.returncode == 0 else 'error'), process.returncode
                except subprocess.TimeoutExpired:
                    status, code = 'timeout', None
            meta = dict(command=command, status=status, exit_code=code,
                        evaluation_seconds=time.perf_counter()-start, **identity)
            meta_path.write_text(json.dumps(meta, indent=2), encoding='utf-8')
        result = dict(case=case, cores=cores, status=meta['status'], exit_code=meta['exit_code'],
                      evaluation_seconds=meta['evaluation_seconds'], **identity,
                      artifacts=folder.relative_to(out).as_posix())
        if meta['status'] == 'ok':
            value = json.loads((folder/'result.json').read_text(encoding='utf-8'))
            assert value['scene'] == 'B' and value['num_cores'] == cores
            assert value['capacity_bytes'] == {'L1': 1572864, 'UB': 131072}
            assert value['bandwidth_bytes_per_cycle'] == 60
            assert value['cross_core_copy_delay_cycles'] == 1000
            result.update(makespan=value['makespan'], data_movement_bytes=value['data_movement_bytes'],
                          memory_peak_by_core=value['memory_peak_by_core'],
                          result_sha256=sha(folder/'result.json'))
        else:
            result['error'] = (folder/'stderr.txt').read_text(encoding='utf-8', errors='replace').strip()
        return result

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(evaluate, row) for row in rows]
        for future in as_completed(futures):
            row = future.result()
            report['rows'].append(row)
            report['rows'].sort(key=lambda item: (item['case'], item['cores']))
            report['batch_wall_seconds'] = time.perf_counter()-started
            (out/'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(json.dumps({k: row[k] for k in ['case', 'cores', 'status']} |
                             ({'makespan': row['makespan']} if row['status'] == 'ok' else
                              {'error': row['error'][:300]}), ensure_ascii=False), flush=True)
    print(f"Completed {len(report['rows'])} official problem-2 evaluations; "
          f"{sum(row['status'] == 'ok' for row in report['rows'])} succeeded.")


if __name__ == '__main__':
    main()
