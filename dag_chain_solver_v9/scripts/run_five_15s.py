"""One measured generation per public input at five cores, then original scoring."""
import argparse
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
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root/'tests'))
    from check_outputs import generate
    from check_rules_outputs import check_plan
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--solver', type=Path, default=root/'build_msvc/bin/npu_sched.exe')
    parser.add_argument('--out', type=Path, default=root/'runs/five_15s')
    parser.add_argument('--stage', choices=['all','generate','evaluate'], default='all')
    args = parser.parse_args()
    out = args.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    solver = args.solver.resolve()
    reference = root/'references/hackathon_20261030'
    manifest = json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
    for name,digest in manifest['files'].items():
        assert sha(reference/name)==digest,('official file changed',name)
    baseline = json.loads((root/'reports/v5_results_baseline.json').read_text(encoding='utf-8'))
    old = {r['case']:r for r in baseline['rows'] if r['cores']==5}
    config = reference/'data/config.txt'
    current = dict(version='0.6.1',core_limit=5,requested_budget_seconds=15.0,
                   solver_executable_sha256=sha(solver),config_sha256=sha(config),
                   source_sha256={q.relative_to(root).as_posix():sha(q) for q in sorted((root/'src').glob('*')) if q.is_file()},
                   official_source_sha256={q.relative_to(reference).as_posix():sha(q) for q in sorted((reference/'code').glob('*.py'))},
                   rows=[],completed=False)
    saved = out/'summary.json'
    if saved.exists():
        prior = json.loads(saved.read_text(encoding='utf-8'))
        for key in ('solver_executable_sha256','source_sha256','official_source_sha256','config_sha256'):
            assert prior[key]==current[key],('stale run directory',key)
        current = prior
    def save():
        saved.write_text(json.dumps(current,indent=2),encoding='utf-8')
    known = {r['case']:r for r in current['rows']}
    graphs = sorted((reference/'data').glob('case_*.json'))
    assert len(graphs)==10 and set(old)=={q.stem for q in graphs}
    if args.stage in ('all','generate'):
        for graph in graphs:
            case=graph.stem;folder=out/case;folder.mkdir(exist_ok=True)
            plan=folder/'plan_n5.json'
            if case in known:
                assert known[case]['input_sha256']==sha(graph) and known[case]['plan_sha256']==sha(plan)
                continue
            timing=generate(solver,graph,5,plan,observe=True)
            check_plan(json.loads(graph.read_text(encoding='utf-8')),json.loads(plan.read_text(encoding='utf-8')),5)
            row=dict(case=case,cores=5,input_sha256=sha(graph),plan_sha256=sha(plan),
                     generation=timing,status='generated',plan=str(plan.relative_to(root)))
            current['rows'].append(row);known[case]=row;save()
            print(f"{case}: generation {timing['seconds']:.3f}s, structure passed",flush=True)
    if args.stage in ('all','evaluate'):
        assert len(current['rows'])==10,'Finish generation first'
        evaluator=reference/'code/multicore_cut_evaluate_problem_2.py'
        env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
        for row in current['rows']:
            if row['status']=='ok':continue
            case=row['case'];folder=out/case;graph=reference/'data'/f'{case}.json';plan=root/row['plan']
            assert sha(plan)==row['plan_sha256']
            command=[sys.executable,str(evaluator),str(graph),str(plan),'--config',str(config),
                     '-o',str(folder/'result.json'),'--trace-output',str(folder/'trace.json'),
                     '--log-output',str(folder/'log.txt')]
            started=time.perf_counter()
            with (folder/'stdout.txt').open('w',encoding='utf-8') as stdout, (folder/'stderr.txt').open('w',encoding='utf-8') as stderr:
                result=subprocess.run(command,stdout=stdout,stderr=stderr,env=env,timeout=600)
            row.update(evaluation_seconds=time.perf_counter()-started,exit_code=result.returncode,
                       evaluation_command=command,status='ok' if result.returncode==0 else 'error')
            if result.returncode==0:
                score=json.loads((folder/'result.json').read_text(encoding='utf-8'))
                assert score['scene']=='B' and score['num_cores']==5
                assert score['capacity_bytes']=={'L1':1572864,'UB':131072}
                assert score['bandwidth_bytes_per_cycle']==60 and score['cross_core_copy_delay_cycles']==1000
                row.update(cycles=score['makespan'],speedup=old[case]['singlecore_cycles']/score['makespan'],
                           singlecore_cycles=old[case]['singlecore_cycles'],data_movement_bytes=score['data_movement_bytes'],
                           memory_peak_by_core=score['memory_peak_by_core'],result_sha256=sha(folder/'result.json'),
                           v5_cycles=old[case]['cycles'],v5_copy_bytes=old[case]['copy_bytes'])
            else:
                row['error']=(folder/'stderr.txt').read_text(encoding='utf-8',errors='replace')
            save()
            print(json.dumps(dict(case=case,status=row['status'],cycles=row.get('cycles'),error=row.get('error')),ensure_ascii=False),flush=True)
        current['completed']=True;save()
        assert all(r['status']=='ok' for r in current['rows']),'Official evaluation failures retained in report'


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
