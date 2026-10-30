"""Compare the C++ spill port to unmodified official Scene B Step1/Step2.

Default: 40 historical v7 plans, 64 generic fixture plans, and 16 deliberately
pressure-heavy adversarial plans. This does not approximate official Step3 cycles.
Defaults check final v9 plans in runs/official_all plus generic synthetic and adversarial fixtures.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def read(p):
    return json.loads(p.read_text(encoding='utf-8'))


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def save(p, obj):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(obj, indent=2), encoding='utf-8')


def adversarial_fixtures():
    def build(n, tensor_defs, uses, direct=()):
        return dict(ops=[dict(id=i, op='COMPUTE', pipe='PIPE_V', cycles=50) for i in range(n)],
                    tensors=[dict(id=t, pos=m, size=size) for t, m, size in tensor_defs],
                    edges=[dict(source=a, target=b) for a,b in uses] +
                          [dict(source=a, target=b, data_size=size) for a,b,size in direct])
    # Same next-use position: active allocation order must break the tie.
    tie=build(5,[(300,'UB',40000),(200,'UB',40000),(100,'UB',40000)],
              [(0,100),(100,4),(1,200),(200,4),(2,300),(300,3)])
    # The external input is clean across two spills; both use its initial backing.
    clean=build(7,[(100,'UB',80000),(101,'UB',80000),(102,'UB',80000),(999,'DDR',80000)],
                [(100,0),(100,3),(100,6),(1,101),(101,2),(4,102),(102,5),
                 (999,9000000),(9000000,100)])
    clean['ops'].append(dict(id=9000000,op='COPY_IN',pipe='PIPE_MTE2',cycles=-1))
    # A later writer invalidates the first backing, forcing a second writeback.
    dirty=build(7,[(102,'UB',80000),(100,'UB',80000),(101,'UB',80000)],
                [(0,100),(3,100),(100,6),(1,101),(101,2),(4,102),(102,5)],[(0,3,0)])
    # Cross-core direct edges, including size zero, and a discarded high-ID COPY.
    direct=build(4,[(100,'DDR',0),(101,'UB',60000),(999,'DDR',0)],
                 [(999,9000000),(9000000,100),(100,0),(2,101),(101,3)],
                 [(0,1,60000),(0,2,0),(1,3,0)])
    direct['ops'].append(dict(id=9000000,op='COPY_IN',pipe='PIPE_MTE2',cycles=-1))
    return {'victim_tie_input_permutation':(tie,[[0,1,2,3,4]]),
            'repeated_clean_backing':(clean,[list(range(7))]),
            'dirty_rewrite_backing':(dirty,[list(range(7))]),
            'direct_zero_and_high_copy_id':(direct,[[0],[1,2,3]])}


def manual_plan(graph, streams, cores):
    compute=[op['id'] for op in graph['ops'] if op['op'] not in ('COPY_IN','COPY_OUT')]
    return dict(node_to_subgraph={str(v):v for v in sorted(compute)},
                core_schedules=copy.deepcopy(streams)+[[] for _ in range(cores-len(streams))])


def official_trace(module, graph, plan):
    """Wrap the original function; never edit its source or replace its algorithm.

    CPython's return profile event exposes pending_spills, whose trigger_step is
    not present in the public spill_records. This adds observation only.
    """
    captures=[]
    original=module.step2_spill_insertion
    def capture(task, seq, capacity):
        pending=[]
        previous=sys.getprofile()
        def observe(frame,event,arg):
            if event=='return' and frame.f_code is original.__code__:
                pending.extend(copy.deepcopy(frame.f_locals.get('pending_spills',[])))
        sys.setprofile(observe)
        try:
            result=original(task,seq,capacity=capacity)
        finally:
            sys.setprofile(previous)
        captures.append((task,seq,result,pending))
        return result
    module.step2_spill_insertion=capture
    try:
        tasks,_,_,traffic,_=module._build_scene_b_tasks(graph,plan,60,{'L1':1572864,'UB':131072})
    finally:
        module.step2_spill_insertion=original
    computed=sorted(op['id'] for op in graph['ops'] if op['op'] not in ('COPY_IN','COPY_OUT'))
    dense={v:i for i,v in enumerate(computed)}
    tensor_dense={t['id']:i for i,t in enumerate(graph['tensors'])}
    cores=[];records=[];capture_index=0
    for c,stream in enumerate(plan['core_schedules']):
        if not stream:
            cores.append(dict(core=c,sequence=[],overflow_checks=0));continue
        task,seq,result,pending=captures[capture_index];capture_index+=1
        ops={o['id']:o for o in task['ops']}
        owner=lambda op: dense[tasks[c]['op_subgraph'][op]] if op is not None else -1
        cores.append(dict(core=c,sequence=[dict(id=op,owner=owner(op),kind=ops[op]['op']) for op in seq],
                          overflow_checks=len(result['overflow_log'])))
        assert len(pending)==len(result['spill_records'])
        for p,rec in zip(pending,result['spill_records']):
            trigger=p['trigger_step'];tid=rec['logical_tid']
            row=dict(core=c,tensor_index=tensor_dense.get(tid,-1),tensor_id=tid,
                     size=rec['size'],memory=0 if rec['pos']=='L1' else 1,
                     trigger_step=trigger,prev_use_step=rec['prev_use_step'],next_use_step=rec['next_use_step'],
                     trigger_owner=owner(seq[trigger]),prev_owner=owner(rec['prev_use_op']),next_owner=owner(rec['next_use_op']),
                     trigger_op=seq[trigger],prev_use_op=rec['prev_use_op'],
                     next_use_op=rec['next_use_op'])
            for key in ('spill_out_copies_data','spill_out_id','spill_in_id','backing_tid','from_tid','to_tid','version','backing_source'):
                row[key]=rec[key]
            records.append(row)
    assert capture_index==len(captures)
    return dict(valid=True,route_bytes=traffic['scheduled_copy_bytes']-traffic['spill_added_copy_bytes'],
                spill_bytes=traffic['spill_added_copy_bytes'],cores=cores,records=records)


def compare(expected, actual):
    assert actual['valid'],actual.get('error')
    for key in ('route_bytes','spill_bytes'):
        assert actual[key]==expected[key],(key,actual[key],expected[key])
    assert len(actual['cores'])==len(expected['cores'])
    for old,new in zip(expected['cores'],actual['cores']):
        for key in ('core','sequence','overflow_checks'):
            if new[key]!=old[key]:
                if key=='sequence':
                    mismatch=next((i for i,(a,b) in enumerate(zip(new[key],old[key])) if a!=b),min(len(new[key]),len(old[key])))
                    raise AssertionError(('core sequence',old['core'],mismatch,new[key][max(0,mismatch-1):mismatch+2],old[key][max(0,mismatch-1):mismatch+2]))
                raise AssertionError(('core',old['core'],key,new[key],old[key]))
    assert len(actual['records'])==len(expected['records']),('record count',len(actual['records']),len(expected['records']))
    for i,(old,new) in enumerate(zip(expected['records'],actual['records'])):
        for key,value in old.items():
            assert key in new and new[key]==value,('record',i,key,new.get(key),value)


def main():
    root=Path(__file__).resolve().parents[1];v7=root.parent/'dag_chain_solver_v7'
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--diagnostics',type=Path,default=root/'build_msvc/bin/spill_diagnostics.exe')
    p.add_argument('--solver',type=Path,default=root/'build_msvc/bin/npu_sched.exe')
    p.add_argument('--run',type=Path,default=root/'runs/official_all')
    p.add_argument('--plan-root',type=Path,default=root)
    p.add_argument('--out',type=Path,default=root/'runs/spill_differential')
    p.add_argument('--groups',nargs='+',choices=['public','synthetic','adversarial'],default=['public','synthetic','adversarial'])
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(root/'tests'));sys.path.insert(0,str(root/'references/hackathon_20261030/code'))
    from run_v5_synthetic import fixtures, validate_fixture
    from check_rules_outputs import check_plan
    import multicore_cut_evaluate_problem_2 as official
    reference=root/'references/hackathon_20261030'
    manifest=read(root/'reports/rules_manifest.json')
    for name,digest in manifest['files'].items():assert sha(reference/name)==digest,('modified official file',name)
    metadata=dict(diagnostics_sha256=sha(a.diagnostics),solver_sha256=sha(a.solver),groups=a.groups,
                  official_manifest_sha256=sha(root/'reports/rules_manifest.json'),plan_root=str(a.plan_root.resolve()),
                  source_sha256={f.relative_to(root).as_posix():sha(f) for f in (root/'src').glob('*') if f.is_file()})
    report_path=out/'report.json'
    report=dict(metadata=metadata,rows=[],completed=False)
    if report_path.exists():
        report=read(report_path);assert report['metadata']==metadata,'Changed binary/source/settings: use a new output directory'
    done={(x['group'],x['case'],x['cores']):x for x in report['rows'] if x['status']=='ok'}
    jobs=[]
    if 'public' in a.groups:
        run=read(a.run/'summary.json');assert len(run['rows'])==40 and run['completed']
        for row in run['rows']:
            graph=reference/'data'/f"{row['case']}.json";plan=a.plan_root/row['plan']
            assert sha(graph)==row['input_sha256'] and sha(plan)==row['plan_sha256']
            jobs.append(('public',row['case'],row['cores'],graph,plan,row.get('data_movement_bytes')))
    if 'synthetic' in a.groups:
        for name,g in fixtures().items():
            validate_fixture(g);graph=out/'inputs'/f'{name}.json';save(graph,g)
            for n in (2,3,4,5):jobs.append(('synthetic',name,n,graph,None,None))
    if 'adversarial' in a.groups:
        for name,(g,streams) in adversarial_fixtures().items():
            validate_fixture(g);graph=out/'inputs'/f'{name}.json';save(graph,g)
            for n in (2,3,4,5):
                plan=out/f'adversarial_{name}_n{n}'/'plan.json';save(plan,manual_plan(g,streams,n))
                jobs.append(('adversarial',name,n,graph,plan,None))
    env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
    for group,name,n,graph_path,plan_path,scored_movement in jobs:
        key=(group,name,n)
        if key in done:
            assert done[key]['input_sha256']==sha(graph_path)
            continue
        folder=out/f'{group}_{name}_n{n}';folder.mkdir(exist_ok=True)
        row=dict(group=group,case=name,cores=n,status='error',input_sha256=sha(graph_path));started=time.perf_counter()
        try:
            graph=read(graph_path)
            if plan_path is None:
                plan_path=folder/'plan.json'
                result=subprocess.run([str(a.solver),str(graph_path),'-n',str(n),'-o',str(plan_path)],capture_output=True,timeout=90,env=env)
                assert result.returncode==0,result.stderr.decode('utf-8',errors='replace')
            plan=read(plan_path);check_plan(graph,plan,n)
            expected=official_trace(official,graph,plan)
            save(folder/'official_trace.json',expected)
            command=[str(a.diagnostics),str(graph_path),str(plan_path),str(folder/'cpp_trace.json')]
            result=subprocess.run(command,capture_output=True,timeout=120,env=env)
            assert result.returncode==0,result.stderr.decode('utf-8',errors='replace')
            actual=read(folder/'cpp_trace.json');compare(expected,actual)
            if scored_movement:
                assert expected['spill_bytes']==scored_movement['spill_added_copy_bytes']
                assert expected['route_bytes']+expected['spill_bytes']==scored_movement['scheduled_copy_bytes']
            if name=='repeated_clean_backing':
                assert len(expected['records'])>=2 and all(not x['spill_out_copies_data'] for x in expected['records'])
            elif name=='dirty_rewrite_backing':
                assert len(expected['records'])>=2 and all(x['spill_out_copies_data'] for x in expected['records'])
            elif name=='victim_tie_input_permutation':
                assert expected['records'] and expected['records'][0]['tensor_id']==100
            row.update(status='ok',plan_sha256=sha(plan_path),sequence_ops=sum(len(x['sequence']) for x in actual['cores']),
                       spill_records=len(actual['records']),spill_bytes=actual['spill_bytes'],route_bytes=actual['route_bytes'],
                       cpp_trace_sha256=sha(folder/'cpp_trace.json'),official_trace_sha256=sha(folder/'official_trace.json'))
        except Exception as error:
            row['error']=repr(error)
        row['seconds']=time.perf_counter()-started
        report['rows']=[x for x in report['rows'] if (x['group'],x['case'],x['cores'])!=key]+[row]
        save(report_path,report)
        print(json.dumps({k:row.get(k) for k in ('group','case','cores','status','spill_records','error')}),flush=True)
        if row['status']!='ok':raise SystemExit(1)
    report.update(completed=True,passed=len(report['rows']),failed=0)
    save(report_path,report)
    print(f"{len(report['rows'])} exact spill differential checks passed",flush=True)


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
