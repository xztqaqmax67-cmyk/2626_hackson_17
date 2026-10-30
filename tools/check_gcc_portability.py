"""Check independently built GCC executables using Unicode/space paths and real plans."""
from pathlib import Path
from benchmark_submissions import ROOT, EVALUATOR, read, save, invoke, digest, check_plan
import json

OUT=ROOT/'output/submission_review_20261030/gcc_check'
OUT.mkdir(parents=True,exist_ok=True)
matrix=[('old',ROOT/'hackathon_v3/build/submission_mingw/bin/npu_sched.exe','case_08',2,
         ROOT/'output/submission_review_20261030/old_final/cores_2/case_08/plan.json'),
        ('new',ROOT/'output/submission_review_20261030/gcc_new_build/bin/npu_sched.exe','case_09',4,
         ROOT/'output/submission_review_20261030/new_writer_fix_smoke/cores_4/case_09/plan.json')]
rows=[]
for variant,exe,name,cores,reference in matrix:
    graph=read(ROOT/'hackathon_data'/f'{name}.json')
    renamed=OUT/variant/'中文 空格'/f'{name} renamed.json'
    save(renamed,graph)
    target=renamed.parent/'new folder'/'plan.json'
    cmd=[exe,renamed,f'--cores={cores}',f'--output={target}']
    run,stdout=invoke(cmd,OUT/variant/'run')
    assert run['returncode']==0 and run['seconds']<90 and stdout==''
    check_plan(graph,read(target),cores)
    before=digest(target)
    repeat,stdout=invoke(cmd,OUT/variant/'repeat',cwd=OUT)
    assert repeat['returncode']==0 and digest(target)==before and stdout==''
    row=dict(variant=variant,case=name,cores=cores,gcc_version='MinGW-w64 GCC 15.1.0, Windows',
             deterministic=True,plan_sha256=before,equal_to_msvc_plan=before==digest(reference),
             seconds=run['seconds'],repeat_seconds=repeat['seconds'])
    request=dict(action='scene_b',graph=graph,plan=read(target),capacity={'L1':1572864,'UB':131072},bandwidth=60,delay=1000)
    evaluation,output=invoke([EVALUATOR],OUT/variant/'evaluate',timeout=240,request=request)
    assert evaluation['returncode']==0
    row['makespan']=json.loads(output)['makespan']
    rows.append(row)
    save(OUT/'results.json',rows)
    print(row,flush=True)
