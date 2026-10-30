"""Exact C++ Step3 differential against unmodified official local/global timelines.

Use --limit for a bounded smoke subset, then omit it to resume the same full120.
Reference source is never modified; a wrapper observes returned local results.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from check_official_spill import adversarial_fixtures, manual_plan


def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v):p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(v,indent=2),encoding='utf-8')


def collision_fixtures():
    # Five inputs become free together. Their CPython set iteration determines
    # which virtual credits the next allocation reuses, including exact IDs.
    modulus=(1<<61)-1
    tids=[modulus,0,modulus+1,1,2*modulus]
    sizes=[12000,17000,19000,23000,29000]
    graph=dict(ops=[dict(id=i,op='COMPUTE',pipe='PIPE_V',cycles=50) for i in range(100,110)],
               tensors=[dict(id=t,pos='UB',size=s) for t,s in zip(tids,sizes)]+
                       [dict(id=100001,pos='UB',size=60000),dict(id=100002,pos='UB',size=70000)],edges=[])
    for i,tid in enumerate(tids):graph['edges'] += [dict(source=100+i,target=tid),dict(source=tid,target=105)]
    graph['edges'] += [dict(source=105,target=106),dict(source=106,target=100001),dict(source=100001,target=107),
                       dict(source=107,target=108),dict(source=108,target=100002),dict(source=100002,target=109)]
    return {'python_hash_collision_credit_reuse':(graph,[list(range(100,110))])}


def precision_fixtures():
    from run_v5_synthetic import graph_from_dag
    cases={}
    for offset in (0,4):
        graph=graph_from_dag([(1<<53)+offset,4],[(0,1)],sizes=[60,60],pipes=['PIPE_M','PIPE_V'])
        cases[f'large_ddr_cursor_offset_{offset}']=(graph,[[0],[1]])
    for offset in (1,5):
        base=(1<<53)+offset
        graph=graph_from_dag([base,base+1,4],[(0,2),(1,2)],sizes=[60,60,60],pipes=['PIPE_M','PIPE_V','PIPE_FIX'])
        cases[f'large_parallel_finish_offset_{offset}']=(graph,[[0,1,2]])
    cases['large_exact_integer_control']=(dict(
        ops=[dict(id=10,op='COMPUTE',pipe='PIPE_M',cycles=(1<<53)+4),
             dict(id=11,op='COMPUTE',pipe='PIPE_V',cycles=4)],
        tensors=[],edges=[dict(source=10,target=11)]),[[10,11]])
    return cases


def official_timing(module,graph,plan):
    captures=[];original=module.prepare_step3_execution
    def observe(*args,**kwargs):
        prepared=original(*args,**kwargs);captures.append(prepared);return prepared
    module.prepare_step3_execution=observe
    try:
        scored=module.evaluate_scene_b(graph,plan,60,{'L1':1572864,'UB':131072},1000)
    finally:module.prepare_step3_execution=original
    assert len(captures)==len(plan['core_schedules'])
    global_ops={x['core_id']:x['ops'] for x in scored['per_core_timeline']}
    cores=[]
    for core,prepared in enumerate(captures):
        step=prepared['step3']
        cores.append(dict(core=core,local_cycles=step['makespan'],peak=step['memory_peak'],
                          pipe_orders=step['pipe_orders'],
                          memory_dependencies=step['memory_dependencies'],
                          local_timeline=[dict(id=op,start=step['op_start'][op],end=step['op_end'][op]) for op in sorted(step['op_start'])],
                          timeline=[dict(id=x['op_id'],start=x['start'],end=x['end']) for x in sorted(global_ops[core],key=lambda x:x['op_id'])]))
    movement=scored['data_movement_bytes']
    return dict(valid=True,cycles=scored['makespan'],route_bytes=movement['scheduled_copy_bytes']-movement['spill_added_copy_bytes'],
                spill_bytes=movement['spill_added_copy_bytes'],cores=cores)


def compare(expected,actual):
    assert actual['valid'],actual.get('error')
    for key in ('cycles','route_bytes','spill_bytes'):
        assert actual[key]==expected[key],(key,actual.get(key),expected[key])
    assert len(actual['cores'])==len(expected['cores'])
    for want,got in zip(expected['cores'],actual['cores']):
        for key in ('core','local_cycles','peak','pipe_orders'):
            assert got[key]==want[key],('core',want['core'],key,got.get(key),want[key])
        def deps(rows):return sorted(rows,key=lambda x:(x['source'],x['target']))
        assert deps(got['memory_dependencies'])==deps(want['memory_dependencies']),('memory_dependencies',want['core'])
        for key in ('local_timeline','timeline'):
            ordered=sorted(got[key],key=lambda x:x['id'])
            assert len(ordered)==len(want[key]),('timeline length',want['core'],key)
            for left,right in zip(want[key],ordered):
                assert all(left[k]==right[k] for k in ('id','start','end')),('timeline',want['core'],key,left,right)


def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--diagnostics',type=Path,default=root/'build_msvc/bin/timing_diagnostics.exe')
    p.add_argument('--solver',type=Path,default=root/'build_msvc/bin/npu_sched.exe')
    p.add_argument('--run',type=Path,default=root/'runs/official_exact')
    p.add_argument('--plan-root',type=Path,default=root)
    p.add_argument('--out',type=Path,default=root/'runs/timing_differential')
    p.add_argument('--groups',nargs='+',choices=['public','synthetic','adversarial','collisions','precision'],default=['public','synthetic','adversarial'])
    p.add_argument('--limit',type=int,help='Execute this many additional jobs, preserving a resumable full job set')
    p.add_argument('--solver-extra',nargs=argparse.REMAINDER,default=[])
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    reference=root/'references/hackathon_20261030'
    sys.path.insert(0,str(root/'tests'));sys.path.insert(0,str(reference/'code'))
    from run_v5_synthetic import fixtures,validate_fixture
    from check_rules_outputs import check_plan
    import multicore_cut_evaluate_problem_2 as official
    for name,digest in read(root/'reports/rules_manifest.json')['files'].items():assert sha(reference/name)==digest,('reference changed',name)
    metadata=dict(diagnostics_sha256=sha(a.diagnostics),solver_sha256=sha(a.solver),solver_extra=a.solver_extra,
                  groups=a.groups,run=str(a.run.resolve()),plan_root=str(a.plan_root.resolve()),
                  checker_sha256=sha(Path(__file__)),
                  fixture_sources_sha256={name:sha(root/name) for name in ('scripts/check_official_spill.py','tests/run_v5_synthetic.py')},
                  source_sha256={f.relative_to(root).as_posix():sha(f) for f in (root/'src').glob('*') if f.is_file()},
                  official_manifest_sha256=sha(root/'reports/rules_manifest.json'))
    path=out/'report.json';report=dict(metadata=metadata,rows=[],completed=False)
    if path.exists():report=read(path);assert report['metadata']==metadata,'Source/binary/settings changed: choose a new out directory'
    done={(r['group'],r['case'],r['cores']):r for r in report['rows'] if r['status']=='ok'}
    jobs=[]
    if 'public' in a.groups:
        run=read(a.run/'summary.json');assert run['completed']
        for row in run['rows']:
            graph=reference/'data'/(row['case']+'.json');plan=a.plan_root/row['plan']
            assert sha(graph)==row['input_sha256'] and sha(plan)==row['plan_sha256']
            jobs.append(('public',row['case'],row['cores'],graph,plan,row))
    if 'synthetic' in a.groups:
        for name,graph in fixtures().items():
            validate_fixture(graph);q=out/'inputs'/(name+'.json');save(q,graph)
            for n in (2,3,4,5):jobs.append(('synthetic',name,n,q,None,None))
    if 'adversarial' in a.groups:
        for name,(graph,streams) in adversarial_fixtures().items():
            validate_fixture(graph);q=out/'inputs'/(name+'.json');save(q,graph)
            for n in (2,3,4,5):
                plan=out/f'adversarial_{name}_n{n}'/'plan.json';save(plan,manual_plan(graph,streams,n))
                jobs.append(('adversarial',name,n,q,plan,None))
    if 'collisions' in a.groups:
        for name,(graph,streams) in collision_fixtures().items():
            validate_fixture(graph);q=out/'inputs'/(name+'.json');save(q,graph)
            for n in (2,3,4,5):
                plan=out/f'collisions_{name}_n{n}'/'plan.json';save(plan,manual_plan(graph,streams,n))
                jobs.append(('collisions',name,n,q,plan,None))
    if 'precision' in a.groups:
        for name,(graph,streams) in precision_fixtures().items():
            validate_fixture(graph);q=out/'inputs'/(name+'.json');save(q,graph)
            for n in (2,3,4,5):
                plan=out/f'precision_{name}_n{n}'/'plan.json';save(plan,manual_plan(graph,streams,n))
                jobs.append(('precision',name,n,q,plan,None))
    env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1');attempted=0
    for group,name,n,graph_path,plan_path,scored in jobs:
        key=group,name,n
        if key in done:
            prev=done[key];assert prev['input_sha256']==sha(graph_path)
            if plan_path is not None:assert prev['plan_sha256']==sha(plan_path)
            assert sha(out/prev['cpp_trace'])==prev['cpp_trace_sha256']
            assert sha(out/prev['official_trace'])==prev['official_trace_sha256']
            continue
        if a.limit is not None and attempted>=a.limit:break
        attempted+=1;folder=out/f'{group}_{name}_n{n}';folder.mkdir(exist_ok=True)
        row=dict(group=group,case=name,cores=n,status='error',input_sha256=sha(graph_path));started=time.perf_counter()
        try:
            graph=read(graph_path)
            if plan_path is None:
                plan_path=folder/'plan.json';run=subprocess.run([str(a.solver.resolve()),str(graph_path),'-n',str(n),'-o',str(plan_path),*a.solver_extra],capture_output=True,timeout=180,env=env)
                assert run.returncode==0,run.stderr.decode('utf-8',errors='replace')
            plan=read(plan_path);check_plan(graph,plan,n)
            before=time.perf_counter()
            try:expected=official_timing(official,graph,plan)
            except Exception as error:
                if group!='precision':raise
                # The pinned official evaluator itself may reject structurally
                # valid large-cycle input because of its floating-point rules.
                expected=dict(valid=False,error=repr(error))
            row['official_seconds']=time.perf_counter()-before
            expected_path=folder/'official_timing.json';save(expected_path,expected)
            actual_path=folder/'cpp_timing.json';before=time.perf_counter()
            run=subprocess.run([str(a.diagnostics.resolve()),str(graph_path),str(plan_path),str(actual_path)],capture_output=True,timeout=180,env=env)
            row['cpp_process_seconds']=time.perf_counter()-before
            assert run.returncode in (0,2),run.stderr.decode('utf-8',errors='replace')
            actual=read(actual_path)
            assert actual['valid']==expected['valid'],('validity mismatch',expected,actual)
            if expected['valid']:compare(expected,actual)
            else:
                assert run.returncode==2 and actual.get('error')
                row.update(official_error=expected['error'],cpp_error=actual['error'])
            if scored:
                assert expected['cycles']==scored['cycles']
                assert expected['spill_bytes']==scored['data_movement_bytes']['spill_added_copy_bytes']
                assert expected['route_bytes']+expected['spill_bytes']==scored['data_movement_bytes']['scheduled_copy_bytes']
            row.update(status='ok',valid=actual['valid'],plan_sha256=sha(plan_path),cycles=actual.get('cycles'),route_bytes=actual.get('route_bytes'),spill_bytes=actual.get('spill_bytes'),
                       operations=sum(len(x['timeline']) for x in actual.get('cores',[])),
                       cpp_trace=actual_path.relative_to(out).as_posix(),cpp_trace_sha256=sha(actual_path),
                       official_trace=expected_path.relative_to(out).as_posix(),official_trace_sha256=sha(expected_path))
        except Exception as error:row['error']=repr(error)
        row['seconds']=time.perf_counter()-started
        report['rows']=[r for r in report['rows'] if (r['group'],r['case'],r['cores'])!=key]+[row]
        save(path,report);print(json.dumps({k:row.get(k) for k in ('group','case','cores','status','cycles','error')}),flush=True)
        if row['status']!='ok':raise SystemExit(1)
    passed=sum(r['status']=='ok' for r in report['rows'])
    report.update(completed=passed==len(jobs),passed=passed,failed=sum(r['status']!='ok' for r in report['rows']),expected=len(jobs),
                  matched_official_rejections=sum(r.get('valid') is False and r['status']=='ok' for r in report['rows']),
                  by_group=dict(Counter(r['group'] for r in report['rows'] if r['status']=='ok')))
    save(path,report);print(json.dumps({k:report[k] for k in ('completed','passed','failed','expected','by_group')}),flush=True)

if __name__=='__main__':main()
