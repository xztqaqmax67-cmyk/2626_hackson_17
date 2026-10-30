"""Warm-start directional screening; never reported as end-to-end solver speed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

WORK=Path(__file__).resolve().parent
ROOT=WORK.parents[1]
def read(p): return json.loads(p.read_text(encoding='utf-8'))
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def write(p,v): p.write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def check_result(result):
    assert result['scene']=='B' and result['num_cores']==5
    assert result['bandwidth_bytes_per_cycle']==60 and result['cross_core_copy_delay_cycles']==1000
    assert result['capacity_bytes']=={'L1':1572864,'UB':131072}

def cached_baseline(plans,case,graph,original,submitted,reference):
    """Reuse only a completed, byte-identical plan under byte-identical rules.

    Paths are never used as evidence of equivalence. Missing/inconsistent proof
    simply falls back to a fresh official evaluation; it never accepts a cache.
    """
    summary=plans/'summary.json'
    result_path=plans/case/'result.json'
    try:
        measured=read(summary)
        if not measured.get('completed') or measured.get('core_limit')!=5:
            return None,'cached measurement incomplete or different core limit'
        if measured.get('config_sha256')!=sha(reference/'data/config.txt'):
            return None,'configuration hash mismatch'
        code_hashes={p.relative_to(reference).as_posix():sha(p) for p in sorted((reference/'code').glob('*.py'))}
        if measured.get('official_source_sha256')!=code_hashes:
            return None,'official evaluator source hash mismatch'
        rows=[r for r in measured['rows'] if r['case']==case]
        if len(rows)!=1: return None,'missing or duplicate cached case'
        row=rows[0]
        if row.get('status')!='ok' or row.get('exit_code')!=0 or row.get('cores')!=5:
            return None,'cached case was not a successful five-core evaluation'
        if row.get('input_sha256')!=sha(graph): return None,'input hash mismatch'
        if row.get('plan_sha256')!=sha(original) or row['plan_sha256']!=sha(submitted):
            return None,'original/submitted plan bytes do not match cached plan'
        if row.get('result_sha256')!=sha(result_path): return None,'official result hash mismatch'
        result=read(result_path)
        check_result(result)
        if row.get('cycles')!=result['makespan'] or row.get('data_movement_bytes')!=result['data_movement_bytes']:
            return None,'cached result metrics disagree with measurement summary'
        return dict(result=result,result_path=result_path,summary_sha256=sha(summary)), 'all provenance hashes match'
    except (OSError,KeyError,ValueError,TypeError,AssertionError) as error:
        return None,'cache proof unavailable: '+type(error).__name__

def shortlist(rows, limit):
    good=[r for r in rows if r['valid']]
    base=next(r for r in good if r['id']==0)
    others=[r for r in good if r['id']]
    chosen=[base]
    if not others: return chosen
    selectors=[min(others,key=lambda r:(r['estimated_cycles'],r['copy_bytes'],r['id'])),
               min(others,key=lambda r:(r['copy_bytes'],r['estimated_cycles'],r['id']))]
    bounded=[r for r in others if r['estimated_cycles']<=base['estimated_cycles']*1.01]
    if bounded: selectors.append(min(bounded,key=lambda r:(r['copy_bytes'],r['estimated_cycles'],r['id'])))
    # Additional spots sample the Pareto frontier instead of repeating near-identical winners.
    frontier=[r for r in others if not any(q['estimated_cycles']<=r['estimated_cycles'] and q['copy_bytes']<=r['copy_bytes'] and
        (q['estimated_cycles']<r['estimated_cycles'] or q['copy_bytes']<r['copy_bytes']) for q in others)]
    selectors.extend(sorted(frontier,key=lambda r:(r['copy_bytes'],r['estimated_cycles'])))
    for r in selectors:
        if len(chosen)>=limit+1: break
        if all(r['id']!=q['id'] for q in chosen):chosen.append(r)
    return chosen

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--driver',type=Path,default=WORK/'fixed_candidates.exe')
    p.add_argument('--cases',nargs='+',default=['03','05','08','07','09'])
    p.add_argument('--family',choices=['lifetime','memory','consumer','all'],default='lifetime')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--plans',type=Path,default=ROOT/'runs/senior_15s')
    p.add_argument('--limit',type=int,default=24)
    p.add_argument('--official-limit',type=int,default=1)
    p.add_argument('--fresh-baseline',action='store_true',help='Score baseline again even if exact cached provenance matches.')
    p.add_argument('--stage',choices=['all','screen','evaluate'],default='all')
    a=p.parse_args()
    if a.official_limit<1: p.error('--official-limit must be positive')
    out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=ROOT/'references/hackathon_20261030'
    manifest=read(ROOT/'reports/rules_manifest.json')
    for name,digest in manifest['files'].items():assert sha(reference/name)==digest,('changed official',name)
    sys.path.insert(0,str(ROOT/'tests'))
    from check_rules_outputs import check_plan
    cases=[x if x.startswith('case_') else 'case_'+x.zfill(2) for x in a.cases]
    assert len(set(cases))==len(cases)
    driver=a.driver.resolve()
    report=dict(experiment='warm_start_fixed_candidates',not_end_to_end_search=True,
                family=a.family,limit_per_family=a.limit,max_new_official_per_case=a.official_limit,
                reuse_baseline=not a.fresh_baseline,
                driver_sha256=sha(driver),script_sha256=sha(Path(__file__)),
                caveat='Starts from saved 0.6.2 plans. Preprocessing and candidate evaluation costs are recorded separately. '
                       'Shortlisted official results screen directions and do not replace a fresh end-to-end benchmark.',
                completed=False,rows=[])
    saved=out/'summary.json'
    if saved.exists():
        prior=read(saved)
        for key in ('family','limit_per_family','max_new_official_per_case','reuse_baseline','driver_sha256','script_sha256'):
            assert prior[key]==report[key],('stale run',key)
        report=prior
    done={r['case']:r for r in report['rows']}
    env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
    for case in cases:
        graph=reference/'data'/(case+'.json'); original=a.plans/case/'plan_n5.json'
        folder=out/case;folder.mkdir(exist_ok=True)
        if case in done:
            row=done[case]
            assert row['input_sha256']==sha(graph) and row['starting_plan_sha256']==sha(original)
        else:
            row=dict(case=case,input_sha256=sha(graph),starting_plan_sha256=sha(original),official=[])
            report['rows'].append(row);done[case]=row
        if a.stage in ('all','screen') and not row.get('screened'):
            start=time.perf_counter()
            r=subprocess.run([str(driver),str(graph), '5',str(original),str(folder),a.family,str(a.limit)],
                             capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=120)
            (folder/'driver_stdout.txt').write_text(r.stdout,encoding='utf-8')
            (folder/'driver_stderr.txt').write_text(r.stderr,encoding='utf-8')
            assert r.returncode==0,r.stderr
            screen=read(folder/'screen.json')
            g=read(graph)
            for candidate in screen['rows']:
                if candidate['valid']:
                    plan=folder/candidate['plan'];check_plan(g,read(plan),5);candidate['plan_sha256']=sha(plan)
            write(folder/'screen.json',screen)
            assert read(folder/'candidate_0.json')==read(original),'warm-start loader changed submitted plan'
            row.update(screened=True,screen_process_seconds=time.perf_counter()-start,
                       screening={k:v for k,v in screen.items() if k!='rows'},
                       shortlist=shortlist(screen['rows'],a.official_limit),screen_sha256=sha(folder/'screen.json'))
            write(saved,report)
            print(case,'screened',len(screen['rows']),'in',round(row['screen_process_seconds'],3),'s',flush=True)
        if a.stage in ('all','evaluate'):
            assert row.get('screened'),'Run screen stage first'
            completed_ids={r['id'] for r in row['official']}
            for candidate in row['shortlist']:
                if candidate['id'] in completed_ids:continue
                plan=folder/candidate['plan'];assert sha(plan)==candidate['plan_sha256']
                trial=folder/('official_'+str(candidate['id']));trial.mkdir(exist_ok=True)
                cached=None
                if candidate['id']==0 and not a.fresh_baseline:
                    cached,reason=cached_baseline(a.plans,case,graph,original,plan,reference)
                    row['baseline_cache_check']=reason
                if cached:
                    result=cached['result']
                    # Preserve the exact result bytes covered by the old hash.
                    (trial/'result.json').write_bytes(cached['result_path'].read_bytes())
                    observed=dict(candidate,status='ok',cycles=result['makespan'],data_movement_bytes=result['data_movement_bytes'],
                                  official_seconds=0.0,result_sha256=sha(trial/'result.json'),reused_official=True,
                                  reused_summary_sha256=cached['summary_sha256'])
                    row['official'].append(observed);write(saved,report)
                    print(case,'baseline reused after exact provenance checks',flush=True)
                    continue
                cmd=[sys.executable,str(reference/'code/multicore_cut_evaluate_problem_2.py'),str(graph),str(plan),
                     '--config',str(reference/'data/config.txt'),'-o',str(trial/'result.json'),
                     '--trace-output',str(trial/'trace.json'),'--log-output',str(trial/'log.txt')]
                start=time.perf_counter()
                with (trial/'stdout.txt').open('w',encoding='utf-8') as stdout,(trial/'stderr.txt').open('w',encoding='utf-8') as stderr:
                    process=subprocess.run(cmd,stdout=stdout,stderr=stderr,env=env,timeout=120)
                assert process.returncode==0,(trial/'stderr.txt').read_text(encoding='utf-8')
                result=read(trial/'result.json')
                check_result(result)
                observed=dict(candidate,status='ok',cycles=result['makespan'],data_movement_bytes=result['data_movement_bytes'],
                              official_seconds=time.perf_counter()-start,result_sha256=sha(trial/'result.json'),reused_official=False)
                row['official'].append(observed);write(saved,report)
                print(case,candidate['label'],'cycles',result['makespan'],'COPY',result['data_movement_bytes']['scheduled_copy_bytes'],flush=True)
            row['evaluated']=True;write(saved,report)
    report['completed']=all(done[case].get('evaluated') for case in cases)
    write(saved,report)

if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
