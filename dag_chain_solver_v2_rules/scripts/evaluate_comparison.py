"""After timing has finished, evaluate serial plans and compare official v3 results."""
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
    v3 = root.parent/'dag_chain_solver_v3'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark', type=Path, default=root/'runs/thread_benchmark_20261030')
    parser.add_argument('--out', type=Path, default=root/'runs/official_comparison_20261030')
    args = parser.parse_args()
    benchmark, out = args.benchmark.resolve(), args.out.resolve()
    measured = json.loads((benchmark/'benchmark.json').read_text(encoding='utf-8'))
    assert measured['completed'], 'never run the evaluator concurrently with timing'
    out.mkdir(parents=True, exist_ok=True)
    assert not (out/'summary.json').exists(), 'use a fresh output directory'
    reference = v3/'references/hackathon_20261030'
    evaluator = reference/'code/multicore_cut_evaluate_problem_2.py'
    config = reference/'data/config.txt'
    previous = json.loads((v3/'reports/official_evaluation_v3_1_20261030.json').read_text(encoding='utf-8'))
    for name, digest in previous['evaluator_source_sha256'].items():
        assert sha(reference/name) == digest
    assert sha(config) == previous['config_sha256']
    expected = {(row['case'],row['cores']):row for row in previous['rows']}
    env = dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
    report = dict(version='0.2.1', comparison_version='0.3.1', scoring_performed=True,
                  config_sha256=sha(config), evaluator_source_sha256=previous['evaluator_source_sha256'],
                  serial_executable_sha256=measured['executable_sha256']['serial'], rows=[])

    def evaluate(row):
        case, n = row['case'], row['cores']
        graph = reference/'data'/f'{case}.json'
        plan = benchmark/f'{case}_n{n}'/'serial.json'
        prior = expected[(case,n)]
        assert sha(graph) == prior['input_sha256'] and sha(plan) == prior['plan_sha256']
        folder = out/f'{case}_n{n}'
        folder.mkdir(exist_ok=False)
        command = [sys.executable,str(evaluator),str(graph),str(plan),'--config',str(config),
                   '-o',str(folder/'result.json'),'--trace-output',str(folder/'trace.json'),
                   '--log-output',str(folder/'log.txt')]
        start = time.perf_counter()
        with (folder/'stdout.txt').open('w',encoding='utf-8') as stdout, \
                (folder/'stderr.txt').open('w',encoding='utf-8') as stderr:
            try:
                process = subprocess.run(command,stdout=stdout,stderr=stderr,env=env,timeout=600)
                status, code = ('ok' if process.returncode == 0 else 'error'), process.returncode
            except subprocess.TimeoutExpired:
                status, code = 'timeout', None
        meta = dict(case=case,cores=n,status=status,exit_code=code,command=command,
                    evaluation_seconds=time.perf_counter()-start,input_sha256=sha(graph),
                    plan_sha256=sha(plan),config_sha256=sha(config),evaluator_sha256=sha(evaluator))
        if status == 'ok':
            result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
            old_path = v3/'runs/official_v3_1_20261030/problem2'/f'{case}_n{n}'/'result.json'
            assert sha(old_path) == prior['result_sha256']
            old = json.loads(old_path.read_text(encoding='utf-8'))
            result.pop('input_plan',None)
            old.pop('input_plan',None)
            meta.update(makespan=result['makespan'],data_movement_bytes=result['data_movement_bytes'],
                        matches_v3_result_excluding_plan_filename=(result==old),
                        speedup_over_supplied_singlecore=prior['singlecore_makespan']/result['makespan'],
                        result_sha256=sha(folder/'result.json'))
        else:
            meta['error'] = (folder/'stderr.txt').read_text(encoding='utf-8',errors='replace').strip()
        (folder/'run.json').write_text(json.dumps(meta,indent=2),encoding='utf-8')
        return meta

    with ThreadPoolExecutor(max_workers=2) as pool:
        for future in as_completed([pool.submit(evaluate,row) for row in measured['rows']]):
            result = future.result()
            report['rows'].append(result)
            report['rows'].sort(key=lambda row:(row['case'],row['cores']))
            (out/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            print(json.dumps({key:result.get(key) for key in
                              ['case','cores','status','makespan','matches_v3_result_excluding_plan_filename']}),flush=True)
    report['completed'] = True
    report['valid'] = sum(row['status']=='ok' for row in report['rows'])
    report['matching_v3'] = sum(row.get('matches_v3_result_excluding_plan_filename',False) for row in report['rows'])
    (out/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f"Official evaluations: {report['valid']}/40 valid, {report['matching_v3']}/40 identical to v3.")


if __name__ == '__main__':
    main()
