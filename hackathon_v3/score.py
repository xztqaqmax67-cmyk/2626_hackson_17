#!/usr/bin/env python3
"""Local benchmark harness: candidate JSON -> (makespan, time, DDR, speedup).

The 40/30/30 problem statement does not define a mapping from raw metrics to
absolute 100-point scores. Accordingly this tool reports the actual metrics and
only provides an explicitly NONOFFICIAL score if targets JSON is supplied.

No non-standard Python packages required.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
import sys
import time
from local_eval import VERSION

HERE = Path(__file__).resolve().parent
LOCAL_RUNNER = HERE / 'local_eval' / 'runner.py'


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def positive_int(x, label):
    if type(x) not in (int, float) or not math.isfinite(x) or x <= 0:
        raise ValueError(f'{label}: must be finite and positive, received {x!r}')
    return float(x)


def ddr_bytes(result):
    # The original evaluator distinguishes the original-graph bytes, newly
    # inserted bytes, and the whole scheduled DDR traffic. This contest scores
    # total DDR traffic, not just added bytes.
    d = result.get('data_movement_bytes') or {}
    for value in (d.get('scheduled_copy_bytes'), result.get('total_ddr_bytes'),
                  result.get('scheduled_copy_bytes')):
        if value is not None:
            value = int(value)
            if value < 0:
                raise ValueError('negative DDR byte count')
            return value
    raise ValueError('missing scheduled total DDR copy bytes')


def run(cmd, *, timeout, cwd=None):
    start = time.perf_counter()
    try:
        process = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                                 errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired as e:
        raise RuntimeError(f'timed out after {timeout}s: {cmd[:2]}') from e
    elapsed = time.perf_counter() - start
    if process.returncode != 0:
        raise RuntimeError(f'process exit={process.returncode}: '
                           f'{(process.stderr or process.stdout)[-1800:]}')
    return elapsed


def local_eval(graph, plan, cfg, out, mode, timeout):
    cmd = [sys.executable, str(LOCAL_RUNNER), '--graph', str(graph),
           '--config', str(cfg), '--output', str(out), '--mode', mode]
    if plan is not None:
        cmd += ['--plan', str(plan)]
    return run(cmd, timeout=timeout)


def official_eval(script, graph, plan, cfg, out, timeout):
    # Use this if a *real* compatible new evaluator is later supplied.
    # The old Python evaluator is NOT compatible with these inputs.
    cmd = [sys.executable, str(script), str(graph)]
    if plan is not None:
        cmd.append(str(plan))
    cmd += ['--config', str(cfg), '-o', str(out),
            '--trace-output', str(out.with_suffix('.trace.json')),
            '--log-output', str(out.with_suffix('.log.txt'))]
    return run(cmd, timeout=timeout)


def score_proxy(summary, targets):
    """A transparent target-based heuristic, *not* Huawei's score.

    speed: 40 * min(1,S/S_target)
    time: 30 * min(1,T_target/T_mean)
    DDR:  30 * min(1,D_target/D_mean)
    """
    s = positive_int(targets['speedup'], 'speedup target')
    t = positive_int(targets['solve_time_seconds'], 'solve time target')
    d = positive_int(targets['ddr_bytes_mean'], 'DDR bytes target')
    if summary['average_speedup'] is None:
        return None
    time_ = summary['mean_solve_seconds']
    ddr = summary['mean_ddr_bytes']
    raw = {
        'speedup': 40 * min(1.0, max(0.0, summary['average_speedup'] / s)),
        'time': 30 * min(1.0, t / time_) if time_ > 0 else 30.0,
        'ddr': 30 * min(1.0, d / ddr) if ddr > 0 else 30.0,
    }
    return {'points': raw, 'total': sum(raw.values()),
            'method': 'NONOFFICIAL target-relative scores; not Huawei grading'}


def main():
    parser = argparse.ArgumentParser(
        description='Hackathon local scorer (raw metrics & optional unofficial scores)')
    parser.add_argument('--data', type=Path, required=True,
                        help='folder containing case_*.json')
    parser.add_argument('--plans', type=Path, default=None,
                        help='folder containing case_XX_multicore_res.json')
    parser.add_argument('--solver', type=Path, default=None,
                        help='optionally run solver to generate plans and measure runtime')
    parser.add_argument('--num-cores', '-n', type=int, default=4)
    parser.add_argument('--solver-time-limit', type=float, default=3)
    parser.add_argument('--solver-iterations', type=int)
    parser.add_argument('--config', type=Path, default=HERE / 'config_LOCAL_ASSUMPTIONS.txt')
    parser.add_argument('--out', type=Path, default=HERE / 'report')
    parser.add_argument('--cases', nargs='*', default=None,
                        help='case IDs e.g. case_01 case_02 (default: all)')
    parser.add_argument('--baseline-file', type=Path,
                        help='optional JSON mapping case_XX to singlecore makespan')
    parser.add_argument('--no-baseline-simulation', action='store_true',
                        help='do not compute missing baselines; average speedup then N/A')
    parser.add_argument('--fresh-baseline', action='store_true')
    parser.add_argument('--timeout', type=float, default=180,
                        help='timeout for one solver/evaluation subprocess in seconds')
    parser.add_argument('--engine', choices=['local'], default='local',
                        help='v3 shared-DDR event simulator with constraint audits')
    parser.add_argument('--eval-script', type=Path, default=None,
                        help='optional real Hackathon problem2 evaluator (if supplied)')
    parser.add_argument('--singlecore-script', type=Path, default=None,
                        help='optional real Hackathon singlecore evaluator')
    parser.add_argument('--targets', type=Path, default=None,
                        help='reference thresholds for an explicitly unofficial 40/30/30 score')
    a = parser.parse_args()
    if not 1 <= a.num_cores <= 64:
        parser.error('--num-cores must be in 1..64')
    if not math.isfinite(a.timeout) or a.timeout <= 0:
        parser.error('--timeout must be finite and positive')
    if not math.isfinite(a.solver_time_limit) or a.solver_time_limit < 0:
        parser.error('--solver-time-limit must be finite and nonnegative')
    if a.solver_iterations is not None and a.solver_iterations < 0:
        parser.error('--solver-iterations must be nonnegative')
    if not a.data.is_dir():
        parser.error(f'not a directory: {a.data}')
    if not a.config.is_file():
        parser.error(f'missing config: {a.config}')
    if a.eval_script and not a.singlecore_script and not a.baseline_file:
        print('NOTE: using a new evaluator without a new baseline evaluator: '
              'singlecore baselines will be unavailable unless supplied.', file=sys.stderr)
        a.no_baseline_simulation = True
    data_dir = a.data.resolve()
    config = a.config.resolve()
    config_sha256 = hashlib.sha256(config.read_bytes()).hexdigest()
    config_sha256 += ('-' + ('external' if a.eval_script else a.engine))
    engine_digest = hashlib.sha256()
    engine_sources = ([a.singlecore_script.resolve()] if a.singlecore_script else
                      sorted((HERE / 'local_eval').glob('*.py')))
    for source in engine_sources:
        engine_digest.update(source.name.encode())
        engine_digest.update(source.read_bytes())
    config_sha256 += '-' + engine_digest.hexdigest()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    plans_dir = (a.plans or out / 'plans').resolve()
    plans_dir.mkdir(parents=True, exist_ok=True)
    raw = out / 'raw'
    raw.mkdir(exist_ok=True)
    graphs = sorted(data_dir.glob('case_*.json'))
    if a.cases:
        wanted = set(a.cases)
        graphs = [p for p in graphs if p.stem in wanted]
        if len(graphs) != len(wanted):
            parser.error(f'unknown case(s): {sorted(wanted - {p.stem for p in graphs})}')
    if not graphs:
        parser.error(f'no case_*.json under {data_dir}')
    baseline_reference = read_json(a.baseline_file) if a.baseline_file else {}
    records = []
    for graph in graphs:
        graph_sha256 = hashlib.sha256(graph.read_bytes()).hexdigest()
        name = graph.stem
        plan = plans_dir / f'{name}_multicore_res.json'
        print(f'[{name}] ', end='', flush=True)
        row = {'case': name, 'valid': False, 'source': 'official-if-script-provided' if a.eval_script else 'unofficial-local',
               'num_cores': a.num_cores, 'singlecore_makespan': None,
               'multicore_makespan': None, 'speedup': None,
               'solve_seconds': None, 'ddr_bytes': None, 'audit_passed': None, 'error': ''}
        try:
            if a.solver:
                solver = a.solver.resolve()
                if not solver.is_file():
                    raise FileNotFoundError(f'compiled solver not found: {solver}')
                command = [str(solver), str(graph), '-n', str(a.num_cores),
                           '--config', str(config), '-o', str(plan),
                           '--time-limit', str(a.solver_time_limit)]
                if a.solver_iterations is not None:
                    command += ['--iterations', str(a.solver_iterations)]
                row['solve_seconds'] = run(command, timeout=a.timeout)
            if not plan.is_file():
                raise FileNotFoundError(f'missing schedule JSON: {plan}')
            plan_data = read_json(plan)
            row['num_cores'] = len(plan_data.get('core_schedules', []))
            if row['num_cores'] < 1:
                raise ValueError('empty core_schedules')
            if a.solver and row['num_cores'] != a.num_cores:
                raise ValueError('solver produced wrong number of cores')
            multi_out = raw / f'{name}_multicore.json'
            multi_out.unlink(missing_ok=True)
            if a.eval_script:
                official_eval(a.eval_script.resolve(), graph, plan, config, multi_out, a.timeout)
            else:
                local_eval(graph, plan, config, multi_out, 'multi', a.timeout)
            multi_res = read_json(multi_out)
            row['audit_passed'] = multi_res.get('audit_passed')
            if not a.eval_script and row['audit_passed'] is not True:
                raise ValueError('local evaluator did not pass its constraint audit')
            row['multicore_makespan'] = positive_int(multi_res['makespan'], 'makespan')
            row['ddr_bytes'] = ddr_bytes(multi_res)
            baseline = baseline_reference.get(name)
            if isinstance(baseline, dict):
                baseline = baseline.get('makespan')
            base_out = raw / f'{name}_singlecore.json'
            if baseline is None and base_out.is_file() and not a.fresh_baseline:
                old = read_json(base_out)
                if (old.get('config_path') == str(config)
                        and old.get('config_sha256') == config_sha256
                        and old.get('input_sha256') == graph_sha256
                        and (a.singlecore_script or old.get('audit_passed') is True)
                        and old.get('input_graph') == graph.name):
                    baseline = old.get('makespan')
            if baseline is None and not a.no_baseline_simulation:
                base_out.unlink(missing_ok=True)
                if a.singlecore_script:
                    official_eval(a.singlecore_script.resolve(), graph, None, config, base_out, a.timeout)
                elif not a.eval_script:
                    local_eval(graph, None, config, base_out, 'single', a.timeout)
                if base_out.is_file():
                    base_result = read_json(base_out)
                    if not a.singlecore_script and base_result.get('audit_passed') is not True:
                        raise ValueError('singlecore baseline did not pass its constraint audit')
                    baseline = base_result.get('makespan')
                    base_result['config_path'] = str(config)
                    base_result['config_sha256'] = config_sha256
                    base_result['input_sha256'] = graph_sha256
                    base_out.write_text(json.dumps(base_result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
            if baseline is not None:
                row['singlecore_makespan'] = positive_int(baseline, 'singlecore makespan')
                row['speedup'] = row['singlecore_makespan'] / row['multicore_makespan']
            row['valid'] = True
            print('makespan={:,.0f} speedup={} solve={}s DDR={:,}B'.format(
                row['multicore_makespan'],
                f"{row['speedup']:.4f}x" if row['speedup'] is not None else 'N/A',
                f"{row['solve_seconds']:.3f}" if row['solve_seconds'] is not None else 'N/A',
                row['ddr_bytes']), flush=True)
        except Exception as e:
            row['error'] = str(e)
            print('FAILED:', row['error'][:240], flush=True)
        records.append(row)
    passed = [r for r in records if r['valid']]
    s_vals = [r['speedup'] for r in passed if r['speedup'] is not None]
    t_vals = [r['solve_seconds'] for r in passed if r['solve_seconds'] is not None]
    bytes_vals = [r['ddr_bytes'] for r in passed]
    summary = {
        'evaluator_version': VERSION,
        'is_official': False,
        'engine': 'user-supplied' if a.eval_script else a.engine,
        'user_provided_evaluation_scripts': bool(a.eval_script),
        'metric_definition': 'arithmetic mean per-case speedup T1/TN; solver wall-time; scheduled DDR COPY bytes',
        'cases_total': len(records), 'cases_valid': len(passed),
        'cases_speedup_available': len(s_vals), 'cases_solve_time_available': len(t_vals),
        'average_speedup': statistics.mean(s_vals) if len(s_vals) == len(records) else None,
        'mean_solve_seconds': statistics.mean(t_vals) if len(t_vals) == len(records) else None,
        'total_solve_seconds': sum(t_vals) if len(t_vals) == len(records) else None,
        'mean_ddr_bytes': statistics.mean(bytes_vals) if len(bytes_vals) == len(records) else None,
        'total_ddr_bytes': sum(bytes_vals) if len(bytes_vals) == len(records) else None,
        'is_complete': len(passed) == len(records),
        'warning': ('Rebuilt v3 local model with explicit memory reuse dependencies; NOT OFFICIAL.'
                    if not a.eval_script else 'Using user-supplied evaluator; verify its competition compatibility.'),
        'weights': {'speedup': 40, 'algorithm_time': 30, 'DDR': 30},
        'absolute_official_points': None,
        'notes': '40/30/30 weights alone cannot define scoring normalization.',
    }
    if summary['is_complete'] and summary['mean_solve_seconds'] is not None and a.targets:
        summary['proxy_score'] = score_proxy(summary, read_json(a.targets))
    result = {'summary': summary, 'cases': records}
    json_path = out / 'summary.json'
    csv_path = out / 'cases.csv'
    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    with csv_path.open('w', newline='', encoding='utf-8-sig') as fh:
        writer = csv.DictWriter(fh, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)
    print('\n=== BENCHMARK SUMMARY ===')
    for k in ('cases_valid','cases_total','average_speedup','mean_solve_seconds','mean_ddr_bytes','total_ddr_bytes'):
        print(f'{k}: {summary.get(k)}')
    print('report:', json_path)
    return 0 if summary['is_complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
