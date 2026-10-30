"""External acceptance/Scene B evaluation. Never included in either submission.

Example: python tools/benchmark_submissions.py --variant old --output output/old_run
Requires the separate research evaluator built in 前辈代码_cpp/build/review_20261030_msvc.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT / '前辈代码_cpp/output/review_20261030_msvc'
def locate_binary(project, name, submission=False):
    suffix = '.exe' if os.name == 'nt' else ''
    builds = ['build/submission_msvc', 'build/submission_release', 'submission/build'] if submission else ['build/review_20261030_msvc', 'build']
    candidates = [ROOT / project / build / directory / (name + suffix)
                  for build in builds for directory in ('bin', 'bin/Release')]
    return next((p for p in candidates if p.is_file()), candidates[0])

EVALUATOR = locate_binary('前辈代码_cpp', 'parity_runner')
DATA = ROOT / 'hackathon_data'
VARIANTS = {'old': 'hackathon_v3', 'new': '前辈代码_cpp'}

def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))

def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def invoke(command, log, timeout=90, request=None, cwd=ROOT):
    log.parent.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    payload = None if request is None else json.dumps(request)
    proc = subprocess.Popen(list(map(str, command)), cwd=cwd,
                            stdin=subprocess.PIPE if payload else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            encoding='utf-8', errors='replace')
    expired = False
    try:
        stdout, stderr = proc.communicate(payload, timeout=timeout)
    except subprocess.TimeoutExpired:
        expired = True
        proc.kill()
        stdout, stderr = proc.communicate()
    elapsed = time.perf_counter() - start
    log.with_suffix('.stdout.txt').write_text(stdout, encoding='utf-8')
    log.with_suffix('.stderr.txt').write_text(stderr, encoding='utf-8')
    record = dict(command=list(map(str, command)), returncode=proc.returncode,
                  timeout=expired, seconds=elapsed)
    save(log.with_suffix('.execution.json'), record)
    return record, stdout

def check_plan(graph, plan, cores):
    assert set(plan) == {'node_to_subgraph', 'core_schedules'}
    eligible = {str(op['id']) for op in graph['ops'] if op['op'] not in ('COPY_IN', 'COPY_OUT')}
    assert set(plan['node_to_subgraph']) == eligible
    assert len(plan['core_schedules']) == cores
    values = list(plan['node_to_subgraph'].values())
    sequence = [b for row in plan['core_schedules'] for b in row]
    assert all(type(b) is int and b >= 0 for b in values + sequence)
    assert len(sequence) == len(set(sequence)) and set(values) == set(sequence)

def load_baseline(name, evaluator, cache):
    """Reuse verified local records, or calculate Scene A once on a fresh checkout."""
    graph = DATA / (name + '.json')
    previous_file = REFERENCE / 'summary.json'
    previous_result = REFERENCE / 'baselines' / name / 'result.json'
    if previous_file.is_file() and previous_result.is_file():
        previous = read(previous_file)
        if previous.get('input_hashes_before', {}).get(graph.name) == digest(graph):
            return read(previous_result), previous_result
    executable = evaluator.parent / ('singlecore_evaluate.exe' if os.name == 'nt' else 'singlecore_evaluate')
    if not executable.is_file():
        raise FileNotFoundError('Build the separate research singlecore_evaluate target first: ' + str(executable))
    folder = cache / name
    metadata = dict(graph_sha256=digest(graph), evaluator_sha256=digest(executable),
                    fixed_hardware={'L1':1572864,'UB':131072,'bandwidth':60,'scene_a_cross_wait':1000,'scene_a_same_wait':100})
    result_file, manifest = folder / 'result.json', folder / 'manifest.json'
    if result_file.is_file() and manifest.is_file() and read(manifest) == metadata:
        return read(result_file), result_file
    cache.mkdir(parents=True, exist_ok=True)
    config = cache / 'evaluation_config.txt'
    config.write_text('[capacity]\nL1 1572864\nUB 131072\n[bandwidth]\nbandwidth 60\n'
                      '[multicore_scene_a]\ntask_cross_core_wait_cycles 1000\ntask_same_core_wait_cycles 100\n'
                      '[multicore_scene_b]\ncross_core_copy_delay_cycles 1000\n',encoding='utf-8')
    run, _ = invoke([executable,graph,'--config',config,'-o',result_file,
                     '--trace-output',folder/'trace.json','--log-output',folder/'evaluation.log'],folder/'run',timeout=300)
    if run['returncode'] or run['timeout']: raise RuntimeError('Single-core baseline failed: ' + name)
    save(manifest,metadata)
    return read(result_file), result_file

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--variant', choices=VARIANTS, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', default=[f'case_{i:02}' for i in range(10)])
    parser.add_argument('--cores', nargs='+', type=int, default=[2, 3, 4, 5])
    parser.add_argument('--solver', type=Path, help='Override the submission executable path')
    parser.add_argument('--evaluator', type=Path, default=EVALUATOR, help='Separate research parity_runner executable')
    parser.add_argument('--baseline-cache', type=Path, default=ROOT/'output/submission_baselines')
    args = parser.parse_args()
    out = args.output.resolve()
    if (out / 'summary.json').exists():
        raise SystemExit('Use a fresh output directory; prior results are preserved.')
    variant = ROOT / VARIANTS[args.variant]
    exe = args.solver.resolve() if args.solver else locate_binary(VARIANTS[args.variant], 'npu_sched', submission=True)
    evaluator = args.evaluator.resolve()
    if not exe.is_file() or not evaluator.is_file():
        raise SystemExit('Build the submission and separate research evaluator, or supply --solver and --evaluator.')
    source_hashes = {str(p.relative_to(variant / 'submission')): digest(p)
                     for p in (variant / 'submission').rglob('*') if p.is_file()}
    inputs = {name: digest(DATA / (name + '.json')) for name in args.cases}
    baselines = {name: load_baseline(name,evaluator,args.baseline_cache.resolve()) for name in args.cases}
    rows = []
    summary = dict(variant=args.variant, fixed_hardware=dict(L1=1572864, UB=131072, bandwidth=60, delay=1000),
                   evaluator=str(evaluator), evaluator_sha256=digest(evaluator),
                   executable_sha256=digest(exe), source_sha256=source_hashes, input_sha256=inputs,
                   baseline_sources={name:str(value[1]) for name,value in baselines.items()}, baseline_is_official=False,
                   score_definition='mean_N mean_case(T1/TN), failed runs = 0; local Scene A baseline')
    started = time.perf_counter()
    for cores in args.cores:
        for name in args.cases:
            graph_file = DATA / (name + '.json')
            graph = read(graph_file)
            folder = out / f'cores_{cores}' / name
            target = folder / 'plan.json'
            baseline, baseline_path = baselines[name]
            assert baseline['capacity_bytes'] == {'L1':1572864, 'UB':131072}
            assert baseline['bandwidth_bytes_per_cycle'] == 60
            row = dict(case=name, cores=cores, valid=False, deterministic=False,
                       singlecore_makespan=baseline['makespan'], baseline_sha256=digest(baseline_path),
                       speedup=0, error='')
            try:
                cmd = [exe, graph_file, '-n', cores, '-o', target]
                run, stdout = invoke(cmd, folder / 'solve')
                row['solve_seconds'] = run['seconds']
                if run['returncode'] or run['timeout'] or run['seconds'] >= 90:
                    raise ValueError(f'solver failed: {run}')
                assert stdout == '', 'solver emitted unexpected stdout'
                before = digest(target)
                plan = read(target)
                check_plan(graph, plan, cores)
                repeat, stdout = invoke(cmd, folder / 'repeat')
                row['repeat_seconds'] = repeat['seconds']
                row['deterministic'] = repeat['returncode'] == 0 and digest(target) == before
                assert row['deterministic'] and not repeat['timeout'] and repeat['seconds'] < 90 and stdout == ''
                row['plan_sha256'] = before
                request = dict(action='scene_b', graph=graph, plan=plan,
                               capacity={'L1':1572864,'UB':131072}, bandwidth=60, delay=1000)
                evaluation, output = invoke([evaluator], folder / 'evaluate', timeout=240, request=request)
                row['evaluation_seconds'] = evaluation['seconds']
                if evaluation['returncode'] or evaluation['timeout']:
                    raise ValueError('Scene B evaluator failed: ' + output[:1000] + (folder / 'evaluate.stderr.txt').read_text(encoding='utf-8')[-500:])
                result = json.loads(output)
                movement = result['data_movement_bytes']
                row.update(valid=True, makespan=result['makespan'],
                           ddr_bytes=movement['scheduled_copy_bytes'], added_copy_bytes=movement['added_copy_bytes'],
                           original_copy_bytes=movement['original_graph_copy_bytes'], spill_bytes=movement['spill_added_copy_bytes'])
                row['speedup'] = baseline['makespan'] / result['makespan']
                row['added_copy_ratio'] = row['added_copy_bytes'] / max(1, row['original_copy_bytes'])
            except Exception as error:
                row['error'] = str(error)
                row['valid'] = False
                row['speedup'] = 0
            rows.append(row)
            save(folder / 'metrics.json', row)
            save(out / 'cases.json', rows)
            print(f"{args.variant} {name} n={cores}: valid={row['valid']} deterministic={row['deterministic']} speedup={row['speedup']:.6f} solve={row.get('solve_seconds',0):.3f}s {row['error']}", flush=True)
    summary['per_core'] = {}
    for cores in args.cores:
        group = [r for r in rows if r['cores'] == cores]
        summary['per_core'][str(cores)] = dict(valid=sum(r['valid'] for r in group), total=len(group),
            mean_speedup=statistics.mean(r['speedup'] for r in group))
    summary.update(num_valid=sum(r['valid'] for r in rows), num_runs=len(rows),
                   num_byte_identical=sum(r['deterministic'] for r in rows),
                   score=statistics.mean(c['mean_speedup'] for c in summary['per_core'].values()),
                   max_solve_seconds=max(max(r.get('solve_seconds',0),r.get('repeat_seconds',0)) for r in rows),
                   inputs_unchanged=all(digest(DATA/(name+'.json')) == sha for name,sha in inputs.items()),
                   wall_seconds=time.perf_counter()-started)
    save(out / 'summary.json', summary)
    fields = sorted(set().union(*(r.keys() for r in rows)))
    with (out / 'cases.csv').open('w',encoding='utf-8-sig',newline='') as stream:
        writer=csv.DictWriter(stream,fieldnames=fields)
        writer.writeheader(); writer.writerows(rows)
    print(json.dumps({k:summary[k] for k in ('num_valid','num_runs','score','max_solve_seconds')},ensure_ascii=True),flush=True)

if __name__ == '__main__':
    main()
