"""Score saved plans using the user's latest evaluator, without modifying it."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import time

ROOT=Path(__file__).resolve().parent
OFFICIAL=ROOT.parent/'hackathon_new/hackathon/code'
CONFIG=ROOT.parent/'hackathon_new/hackathon/data/config.txt'
sys.path.insert(0,str(OFFICIAL))
from evaluation_validation import read_evaluation_config
from multicore_cut_evaluate_problem_2 import evaluate_scene_b,read_scene_b_config
from singlecore_evaluate import evaluate_singlecore
from multicore_cut_evaluate_problem_1 import read_scene_a_config

def dump(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf-8')

def main():
    settings=read_evaluation_config(str(CONFIG))
    delay=read_scene_b_config(str(CONFIG))['cross_core_copy_delay_cycles']
    wait=read_scene_a_config(str(CONFIG))
    sources={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(OFFICIAL.glob('*.py'))}
    sources['config.txt']=hashlib.sha256(CONFIG.read_bytes()).hexdigest()
    engine=hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest()
    dump(ROOT/'latest_official/provenance.json',{'files':sources,'engine':engine,'settings':settings,'delay':delay})
    index=[]
    for case in sorted((ROOT.parent/'hackathon_data').glob('*.json')):
        graph=json.loads(case.read_text(encoding='utf-8'))
        graph_hash=hashlib.sha256(case.read_bytes()).hexdigest()
        targets=[('singlecore',None)]
        targets.extend((str(p.relative_to(ROOT)),p) for p in sorted((ROOT/'results').glob(f'*/search_*s_n4/{case.stem}/0/plan.json')))
        for name,path in targets:
            plan=json.loads(path.read_text(encoding='utf-8')) if path else None
            signature=hashlib.sha256((engine+graph_hash+json.dumps(plan,sort_keys=True)).encode()).hexdigest()
            cache=ROOT/'latest_official/cache'/f'{signature}.json'
            if cache.exists():
                summary=json.loads(cache.read_text(encoding='utf-8'))
            else:
                start=time.perf_counter(); errors=io.StringIO()
                try:
                    with contextlib.redirect_stderr(errors):
                        if plan is None:
                            result=evaluate_singlecore(graph,**settings,
                                cross_core_wait=wait['task_cross_core_wait_cycles'],
                                same_core_wait=wait['task_same_core_wait_cycles'])
                        else:
                            result=evaluate_scene_b(graph,plan,**settings,cross_core_copy_delay=delay)
                    summary={'status':'ok','makespan':result['makespan'],'data_movement_bytes':result['data_movement_bytes'],
                             'num_cores':result['num_cores'],'memory_peak_by_core':result.get('memory_peak_by_core')}
                except (ValueError,RuntimeError) as error:
                    summary={'status':'invalid','error':str(error),'error_type':type(error).__name__}
                summary.update(case=case.stem,engine=engine,graph_sha256=graph_hash,plan_sha256=signature,
                               evaluation_seconds=time.perf_counter()-start,stderr=errors.getvalue())
                dump(cache,summary)
            row={'case':case.stem,'source':name,**summary}
            index.append(row)
            dump(ROOT/'latest_official/scores.json',index)
            print(json.dumps({'case':case.stem,'source':name,'status':summary['status'],
                              'makespan':summary.get('makespan'),'seconds':round(summary['evaluation_seconds'],3),
                              'error':summary.get('error','')[:180]},ensure_ascii=False),flush=True)
    print('DONE',len(index),'plan/reference records',flush=True)

if __name__=='__main__':main()
