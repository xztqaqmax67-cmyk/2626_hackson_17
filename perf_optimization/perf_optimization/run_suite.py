import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent
DATA = ROOT.parent / 'hackathon_data'

def main():
    p = argparse.ArgumentParser()
    p.add_argument('stage')
    p.add_argument('--modes', default='fixed,search')
    p.add_argument('--budget', type=float, default=20)
    p.add_argument('--cases', default='')
    p.add_argument('--snapshot', action='store_true')
    p.add_argument('--repeat', type=int, default=1)
    p.add_argument('--cores', type=int, default=4)
    p.add_argument('--plan-stage')
    args = p.parse_args()
    stage = ROOT / 'versions' / args.stage
    if args.snapshot:
        (stage / 'bin').mkdir(parents=True, exist_ok=True)
        for name in ('perf_runner.exe', 'npu_sched.exe'):
            shutil.copy2(ROOT / 'build/bin' / name, stage / 'bin' / name)
        shutil.copytree(ROOT / 'source/problem2', stage / 'problem2', dirs_exist_ok=True)
        manifest = {str(f.relative_to(ROOT/'source')):hashlib.sha256(f.read_bytes()).hexdigest()
                    for f in (ROOT/'source/problem2').rglob('*') if f.is_file()}
        (stage/'source_hashes.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
    cases = sorted(DATA.glob('*.json'))
    if args.cases:
        chosen = set(args.cases.split(','))
        cases = [c for c in cases if c.stem in chosen]
    for mode in args.modes.split(','):
        for case in cases:
            for repeat in range(args.repeat):
                dest = ROOT/'results'/args.stage/f'{mode}_{args.budget:g}s_n{args.cores}'/case.stem/str(repeat)
                dest.mkdir(parents=True,exist_ok=True)
                summary = dest/'summary.json'
                if summary.exists():
                    print('skip', args.stage, mode, case.stem, repeat, flush=True)
                    continue
                request = {'graph':str(case),'action':mode,'cores':args.cores,
                           'budget':args.budget,'output':str(dest/'plan.json')}
                if mode in ('replay','verify'):
                    request['action'] = 'evaluate'
                    source = args.plan_stage or 'v0_baseline'
                    request['plan'] = str(ROOT/'results'/source/f'search_{args.budget:g}s_n{args.cores}'/case.stem/'0/plan.json')
                request_path = dest/'request.json'
                request_path.write_text(json.dumps(request),encoding='utf-8')
                start = time.perf_counter()
                try:
                    completed = subprocess.run([str(stage/'bin/perf_runner.exe'),str(request_path)],
                                               capture_output=True,text=True,encoding='utf-8',
                                               timeout=max(180,args.budget+60))
                    (dest/'stderr.txt').write_text(completed.stderr,encoding='utf-8')
                    try:
                        result=json.loads(completed.stdout)
                    except ValueError:
                        result={'error':'invalid runner output','stdout':completed.stdout[:2000]}
                    result['exit_code']=completed.returncode
                except subprocess.TimeoutExpired:
                    result={'error':'external timeout','exit_code':-1}
                result.update(stage=args.stage,case=case.stem,mode=mode,budget=args.budget,
                              cores=args.cores,repeat=repeat,wall_seconds=time.perf_counter()-start)
                summary.write_text(json.dumps(result,indent=2),encoding='utf-8')
                report=result.get('report',{})
                print(json.dumps({'stage':args.stage,'case':case.stem,'mode':mode,
                                  'wall':round(result['wall_seconds'],3),
                                  'makespan':result.get('makespan',report.get('makespan')),
                                  'evals':report.get('stats',{}).get('full_evaluations'),
                                  'status':result.get('error',report.get('status','ok'))},ensure_ascii=False),flush=True)

if __name__=='__main__': main()
