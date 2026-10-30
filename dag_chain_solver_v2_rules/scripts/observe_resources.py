"""Separate resource observation after the generator timing benchmark has ended."""
import hashlib
import json
from pathlib import Path
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    v3 = root.parent/'dag_chain_solver_v3'
    benchmark = root/'runs/thread_benchmark_20261030'
    recorded = json.loads((benchmark/'benchmark.json').read_text(encoding='utf-8'))
    assert recorded['completed'], 'resource polling must not overlap formal timing'
    sys.path.insert(0,str(v3/'tests'))
    from check_outputs import generate
    out = root/'runs/resource_observations_20261030'
    out.mkdir(parents=True,exist_ok=True)
    report = dict(sample_period_seconds=0.05, used_in_timing_statistics=False,
                  process_threads_include_runtime_threads=True,
                  peak_observations_are_not_worst_case_guarantees=True, rows=[])
    for case in ['case_05','case_07']:
        expected = next(row for row in recorded['rows'] if row['case']==case and row['cores']==5)
        graph = v3/'references/hackathon_20261030/data'/f'{case}.json'
        for version, folder in [('serial',root),('parallel',v3)]:
            executable = folder/'build/bin/npu_sched.exe'
            assert hashlib.sha256(executable.read_bytes()).hexdigest() == recorded['executable_sha256'][version]
            plan = out/f'{case}_n5_{version}.json'
            sampled = generate(executable,graph,5,plan,observe=True)
            assert hashlib.sha256(plan.read_bytes()).hexdigest() == expected['plan_sha256']
            report['rows'].append(dict(case=case,cores=5,version=version,observations=sampled))
            print(json.dumps(report['rows'][-1]),flush=True)
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')


if __name__ == '__main__':
    main()
