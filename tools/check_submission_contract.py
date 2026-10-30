"""Exercise submission v3.0 CLI and I/O contracts outside the deliverable packages."""
import json
from pathlib import Path
import re
import subprocess
from benchmark_submissions import ROOT, EVALUATOR, read, save, digest, invoke, check_plan

OUT = ROOT / 'output/submission_review_20261030/contract'
OUT.mkdir(parents=True, exist_ok=True)
document = (ROOT / '材料/提交要求文档.md').read_text(encoding='utf-8').split('## 附录 A 最小示例',1)[1]
minimal = json.loads(re.search(r'```json\s*(.*?)```',document,re.S)[1])
graph_file = OUT / '中文 路徑' / 'minimal graph.json'
save(graph_file,minimal)
invalid = OUT / 'broken.json'
invalid.write_text('{broken',encoding='utf-8')
blocker = OUT / 'not_a_directory'
blocker.write_text('file',encoding='utf-8')
summary = {}
for variant, project in [('old','hackathon_v3'),('new','前辈代码_cpp')]:
    exe=ROOT/project/'build/submission_msvc/bin/npu_sched.exe'
    folder=OUT/variant
    folder.mkdir(parents=True,exist_ok=True)
    rows=[]
    def test(name,args,code,cwd=OUT):
        result=subprocess.run([str(exe),*map(str,args)],cwd=cwd,stdin=subprocess.DEVNULL,
                              capture_output=True,text=True,encoding='utf-8',timeout=90)
        row=dict(test=name,expected=code,actual=result.returncode,passed=result.returncode==code,
                 stdout=result.stdout,stderr=result.stderr)
        if code not in (0,) and result.stdout: row['passed']=False
        rows.append(row)
        assert row['passed'],row
        return result
    test('version',['--version'],0)
    test('missing_input',[],2)
    for arg in ('--config','--time-limit','--unknown'):
        test('reject_'+arg,[graph_file,arg,'1'],2)
    for core in ('0','1','6','4junk','4.0','-2'):
        test('invalid_core_'+core,[graph_file,'-n',core],2)
    test('missing_core',[graph_file,'-n'],2)
    test('missing_output',[graph_file,'--output'],2)
    test('empty_output',[graph_file,'--output='],2)
    test('duplicate_input',[graph_file,graph_file],2)
    test('duplicate_core',[graph_file,'-n','2','--cores=3'],2)
    test('missing_file',[OUT/'absent.json'],3)
    test('bad_json',[invalid],3)
    test('protect_input',[graph_file,'-o',graph_file],2)
    test('output_directory_is_file',[graph_file,'-o',blocker/'out.json'],4)
    test('output_is_directory',[graph_file,'-o',folder],4)
    default=graph_file.with_name(graph_file.stem+'_multicore_res.json')
    test('default_cores_and_output',[graph_file],0)
    check_plan(minimal,read(default),4)
    default.unlink()  # Only this known temporary test result.
    for cores in (2,3,4,5):
        plan=folder/f'nested 空格/{cores}/plan.json'
        args=[graph_file,f'--cores={cores}',f'--output={plan}']
        test('equals_syntax_'+str(cores),args,0)
        before=digest(plan)
        test('different_cwd_'+str(cores),args,0,cwd=ROOT)
        assert digest(plan)==before
        check_plan(minimal,read(plan),cores)
        request=dict(action='scene_b',graph=minimal,plan=read(plan),capacity={'L1':1572864,'UB':131072},bandwidth=60,delay=1000)
        run,stdout=invoke([EVALUATOR],folder/f'minimal_{cores}',request=request)
        result=json.loads(stdout)
        assert run['returncode']==0 and result['makespan']==6 and result['data_movement_bytes']['added_copy_bytes']==0
    target=folder/'unchanged.json'
    args=[graph_file,'-n','4','-o',target]
    test('config_before',args,0)
    before=digest(target)
    poison_paths=[graph_file.parent/'config.txt',OUT/'config.txt']
    for path in poison_paths: path.write_text('THIS CONFIG MUST NEVER BE READ\nL1=0\nUB=0\n',encoding='utf-8')
    test('ignore_conflicting_config',args,0)
    assert digest(target)==before
    for path in poison_paths: path.unlink()  # Created above, confined to this test directory.
    renamed=folder/'arbitrary different name.json'
    save(renamed,minimal)
    test('renamed_input',[renamed,'-n','4','-o',target],0)
    assert digest(target)==before
    extended=json.loads(json.dumps(minimal))
    extended['ops'][1]['pipe']='PIPE_FIX'
    extended['ops'][0]['cycles']=extended['ops'][2]['cycles']=-1
    extended_file=folder/'fix copy sentinel.json'
    save(extended_file,extended)
    test('pipe_fix_and_copy_minus_one',[extended_file,'-n','4','-o',target],0)
    request=dict(action='scene_b',graph=extended,plan=read(target),capacity={'L1':1572864,'UB':131072},bandwidth=60,delay=1000)
    run,stdout=invoke([EVALUATOR],folder/'extended',request=request)
    assert run['returncode']==0 and json.loads(stdout)['makespan']==6
    summary[variant]=dict(passed=len(rows),total=len(rows),tests=rows,
                          minimal_scene_b_makespan=6,minimal_added_copy_bytes=0)
    save(OUT/'results.json',summary)
    print(f'{variant}: {len(rows)}/{len(rows)} CLI tests passed; minimal example N=2..5 verified',flush=True)
