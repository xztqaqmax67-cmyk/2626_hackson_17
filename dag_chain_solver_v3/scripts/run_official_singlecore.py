"""Run the supplied singlecore_evaluate.py unchanged, preserving each result."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--official-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--workers', type=int, choices=[1, 2], default=2)
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    official = args.official_root.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    evaluator = official/'code/singlecore_evaluate.py'
    config = official/'data/config.txt'
    graphs = sorted((official/'data').glob('case_*.json'))
    assert len(graphs) == 10, 'expected the ten supplied cases'
    assert all(not (out/p.stem).exists() for p in graphs), 'do not overwrite previous runs'
    source_hashes = {p.relative_to(official).as_posix(): sha256(p)
                     for p in sorted((official/'code').glob('*.py'))}
    env = dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
    begin = time.perf_counter()
    summary = dict(started_utc=datetime.now(timezone.utc).isoformat(),
                   baseline='unmodified supplied singlecore_evaluate.py',
                   not_a_claim_about_private_organizer_baseline=True,
                   official_root=str(official), config_sha256=sha256(config),
                   evaluator_source_sha256=source_hashes,
                   python_executable=sys.executable, workers=args.workers,
                   evaluator_timeout_seconds=args.timeout, rows=[])

    def save_summary():
        summary['elapsed_wall_seconds'] = time.perf_counter()-begin
        tmp = out/'summary.json.tmp'
        tmp.write_text(json.dumps(summary, indent=2), encoding='utf-8')
        tmp.replace(out/'summary.json')

    def run(graph):
        case_out = out/graph.stem
        case_out.mkdir()
        result_path = case_out/'result.json'
        command = [sys.executable, str(evaluator), str(graph), '--config', str(config),
                   '-o', str(result_path), '--trace-output', str(case_out/'trace.json'),
                   '--log-output', str(case_out/'log.txt')]
        record = dict(case=graph.stem, command=command, input_sha256=sha256(graph),
                      config_sha256=sha256(config), evaluator_sha256=sha256(evaluator),
                      result_path=str(result_path), status='running')
        start = time.perf_counter()
        try:
            with (case_out/'stdout.txt').open('wb') as stdout, (case_out/'stderr.txt').open('wb') as stderr:
                completed = subprocess.run(command, cwd=official, env=env,
                                           stdout=stdout, stderr=stderr, timeout=args.timeout)
            record['exit_code'] = completed.returncode
            record['status'] = 'ok' if completed.returncode == 0 else 'failed'
            if completed.returncode == 0:
                result = json.loads(result_path.read_text(encoding='utf-8'))
                record['makespan'] = result['makespan']
                record['data_movement_bytes'] = result.get('data_movement_bytes', {})
                record['memory_peak_by_core'] = result.get('memory_peak_by_core', {})
                record['result_sha256'] = sha256(result_path)
        except subprocess.TimeoutExpired:
            record.update(status='timeout', exit_code=None)
        except Exception as error:
            record.update(status='runner_error', exit_code=None, error=str(error))
        record['evaluation_seconds'] = time.perf_counter()-start
        if record['status'] != 'ok':
            record['stderr_tail'] = (case_out/'stderr.txt').read_text(encoding='utf-8', errors='replace')[-4000:]
        (case_out/'run.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
        return record

    def record_result(record):
        summary['rows'].append(record)
        summary['rows'].sort(key=lambda row: row['case'])
        save_summary()
        print(json.dumps({key: record.get(key) for key in
                          ['case', 'status', 'makespan', 'evaluation_seconds']}), flush=True)

    save_summary()
    pilot = next(path for path in graphs if path.stem == 'case_01')
    record_result(run(pilot))
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(run, graph) for graph in graphs if graph != pilot]
        for future in as_completed(futures):
            record_result(future.result())
    summary['completed'] = True
    summary['successes'] = sum(row['status'] == 'ok' for row in summary['rows'])
    summary['failures'] = len(summary['rows'])-summary['successes']
    summary['sum_evaluation_seconds'] = sum(row['evaluation_seconds'] for row in summary['rows'])
    summary['official_sources_unchanged'] = all(
        sha256(official/name) == digest for name, digest in source_hashes.items())
    assert summary['official_sources_unchanged']
    save_summary()
    print(f"Completed {summary['successes']}/{len(graphs)} single-core evaluations; summary={out/'summary.json'}", flush=True)
    return 0 if not summary['failures'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
