"""Paired short search experiments. Resume is a direction screen, not end-to-end timing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--probe',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--seconds',type=float,default=3)
    p.add_argument('--mode',choices=['resume','full'],default='resume')
    p.add_argument('--cases',nargs='+',default=['03','05','08','07'])
    p.add_argument('--policies',nargs='+',type=int,choices=[0,1],default=[0,1],
                   help='0: legacy memory order; 1: useful memory orders first (default solver policy)')
    p.add_argument('--seed-root',type=Path,default=root/'runs/senior_15s')
    p.add_argument('--cache-runs',type=Path,nargs='*',default=[root/'runs/senior_15s'])
    p.add_argument('--build-manifest',type=Path,help='Optional source/build manifest captured when this executable was built')
    args=p.parse_args()
    exe=args.probe.resolve(); out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=root/'references/hackathon_20261030';config=reference/'data/config.txt'
    manifest=json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
    assert all(sha(reference/name)==digest for name,digest in manifest['files'].items())
    sys.path.insert(0,str(root/'tests'))
    from check_rules_outputs import check_plan
    summary=dict(mode=args.mode,requested_seconds=args.seconds,cores=5,
                 meaning='Resume excludes frozen-seed preparation; use full mode for end-to-end evidence.',
                 executable_sha256=sha(exe),config_sha256=sha(config),
                 official_source_sha256={q.name:sha(q) for q in (reference/'code').glob('*.py')},rows=[],completed=False)
    saved=out/'summary.json'
    assert not saved.exists(),'Use a fresh run directory'
    if args.build_manifest:
        build=json.loads(args.build_manifest.read_text(encoding='utf-8'))
        assert build['executable_sha256']==summary['executable_sha256'],'Manifest belongs to another executable'
        summary['build_manifest_sha256']=sha(args.build_manifest)
        (out/'build_manifest.json').write_bytes(args.build_manifest.read_bytes())
    def save(): saved.write_text(json.dumps(summary,indent=2),encoding='utf-8')
    env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
    for i,case in enumerate(args.cases):
        case='case_'+case if not case.startswith('case_') else case
        graph=reference/'data'/(case+'.json')
        seed=args.seed_root.resolve()/case/'plan_n5.json'
        policies=args.policies if i%2==0 else args.policies[::-1]
        for policy in policies:
            folder=out/case/str(policy);folder.mkdir(parents=True)
            plan=folder/'plan.json'
            cmd=[str(exe),str(graph),str(5),str(args.seconds),str(policy),args.mode,str(plan)]
            if args.mode=='resume':cmd.append(str(seed))
            started=time.perf_counter()
            r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',env=env,timeout=90)
            generation_wall_seconds=time.perf_counter()-started
            if r.returncode: raise RuntimeError(r.stderr)
            stats=json.loads(r.stdout)
            check_plan(json.loads(graph.read_text(encoding='utf-8')),
                       json.loads(plan.read_text(encoding='utf-8')),5)
            row=dict(case=case,policy=policy,input_sha256=sha(graph),plan_sha256=sha(plan),
                     wall_seconds=generation_wall_seconds,stats=stats,status='generated',
                     plan=str(plan.relative_to(out)))
            if args.mode=='resume':row['seed_sha256']=sha(seed)
            summary['rows'].append(row);save()
            print(case,policy,'generated',round(row['wall_seconds'],3),stats['unique'],stats['refined'],flush=True)
    # Reuse only a completed evaluation of the exact input and plan, with the
    # same official sources/config and a verified result digest.
    seen={}
    for cache in args.cache_runs:
        prior_path=cache/'summary.json'
        if not prior_path.exists(): continue
        prior=json.loads(prior_path.read_text(encoding='utf-8'))
        if not prior.get('completed') or prior.get('config_sha256')!=summary['config_sha256']: continue
        prior_sources={Path(k).name:v for k,v in prior.get('official_source_sha256',{}).items()}
        if prior_sources!=summary['official_source_sha256']: continue
        for old in prior['rows']:
            if old.get('status')!='ok':continue
            folder=cache/old['case']
            if 'policy' in old:folder=folder/str(old['policy'])
            result=folder/'result.json'
            if not result.exists() or sha(result)!=old.get('result_sha256'):continue
            seen[(old['input_sha256'],old['plan_sha256'])]=json.loads(result.read_text(encoding='utf-8'))
    for row in summary['rows']:
        folder=out/row['case']/str(row['policy']); graph=reference/'data'/(row['case']+'.json')
        key=(row['input_sha256'],row['plan_sha256'])
        if key in seen:
            score=seen[key]; row['reused_identical_plan_evaluation']=True
            (folder/'result.json').write_text(json.dumps(score,indent=2),encoding='utf-8')
        else:
            cmd=[sys.executable,str(reference/'code/multicore_cut_evaluate_problem_2.py'),str(graph),
                 str(out/row['plan']),'--config',str(config),'-o',str(folder/'result.json'),
                 '--log-output',str(folder/'official.log')]
            scoring_started=time.perf_counter()
            r=subprocess.run(cmd,capture_output=True,text=True,encoding='utf-8',env=env,timeout=600)
            row['official_evaluation_seconds']=time.perf_counter()-scoring_started
            if r.returncode: raise RuntimeError(r.stderr)
            score=json.loads((folder/'result.json').read_text(encoding='utf-8'));seen[key]=score
        assert score['scene']=='B' and score['num_cores']==5
        row.update(status='ok',cycles=score['makespan'],copy_bytes=score['data_movement_bytes']['scheduled_copy_bytes'],
                   movement=score['data_movement_bytes'],result_sha256=sha(folder/'result.json'))
        save(); print(row['case'],row['policy'],row['cycles'],row['copy_bytes'],flush=True)
    summary['completed']=True;save()

if __name__=='__main__':main()
