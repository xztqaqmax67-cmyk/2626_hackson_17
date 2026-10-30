import json
import subprocess
import sys
import time
from pathlib import Path

ROOT=Path(__file__).resolve().parent
STAGES=['v0_baseline','v1_json','v2_spans','v3_scalar','v4_scores','v5_cache','v6_pipes']

def run(*args):
    print('RUN',*args,flush=True)
    subprocess.run([sys.executable,'-B',str(ROOT/args[0]),*args[1:]],check=True)

def main():
    for stage in STAGES[1:]:run('check_stage.py',stage)
    for stage in STAGES:
        run('run_suite.py',stage,'--modes','replay','--cases','case_09')
    baseline=json.loads((ROOT/'results/v0_baseline/replay_20s_n4/case_09/0/summary.json').read_text())
    assert baseline.get('exit_code')==0,baseline
    for stage in STAGES[1:]:
        r=json.loads((ROOT/'results'/stage/'replay_20s_n4/case_09/0/summary.json').read_text())
        assert r.get('result_sha256')==baseline['result_sha256'],(stage,r)
    print('PASS case_09 full-result parity across all stages',flush=True)
    for runner in ('v0_baseline','v6_pipes'):
        run('run_suite.py',runner,'--modes','verify','--plan-stage','v6_pipes')
    verified=[]
    for case in sorted((ROOT.parent/'hackathon_data').glob('*.json')):
        a=json.loads((ROOT/'results/v0_baseline/verify_20s_n4'/case.stem/'0/summary.json').read_text())
        b=json.loads((ROOT/'results/v6_pipes/verify_20s_n4'/case.stem/'0/summary.json').read_text())
        search=json.loads((ROOT/'results/v6_pipes/search_20s_n4'/case.stem/'0/summary.json').read_text())['report']
        assert a.get('exit_code')==b.get('exit_code')==0,(case,a,b)
        assert a['result_sha256']==b['result_sha256'],case
        assert a['makespan']==search['makespan'],case
        assert a['movement']==search['data_movement_bytes'],case
        verified.append(case.stem)
    (ROOT/'verified.json').write_text(json.dumps({'baseline_verified_final_plans':verified,'all_stages_case09_equal':True},indent=2))
    print('PASS final plans independently verified by untouched baseline evaluator',flush=True)
    for stage in ('v0_baseline','v6_pipes'):
        run('run_suite.py',stage,'--modes','search','--budget','80','--cases','case_05,case_07,case_08')
    for stage in ('v0_baseline','v6_pipes'):
        run('run_suite.py',stage,'--modes','fixed','--repeat','3')
    compare_core()
    run('summarize_latest.py')

def compare_core():
    results=[]
    baseline=ROOT/'versions/v0_baseline/bin/parity_runner.exe'
    final=ROOT/'build/bin/parity_runner.exe'
    for name in ('case_01','case_03','case_08'):
        print('CORE PARITY',name,flush=True)
        graph=json.loads((ROOT.parent/'hackathon_data'/f'{name}.json').read_text(encoding='utf-8'))
        payload=json.dumps({'action':'core','graph':graph,'cores':4,'capacity':{'L1':1572864,'UB':131072},'bandwidth':60})
        rows=[]
        for exe in (baseline,final):
            begin=time.perf_counter()
            proc=subprocess.run([str(exe)],input=payload,capture_output=True,text=True,encoding='utf-8',timeout=180)
            assert proc.returncode==0,(name,proc.stdout[:500],proc.stderr)
            rows.append((json.loads(proc.stdout),time.perf_counter()-begin))
        assert rows[0][0]==rows[1][0],name
        results.append({'case':name,'identical':True,'baseline_seconds':rows[0][1],'final_seconds':rows[1][1]})
        print('PASS',results[-1],flush=True)
    (ROOT/'construction_parity.json').write_text(json.dumps(results,indent=2),encoding='utf-8')

if __name__=='__main__':main()
