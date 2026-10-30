"""Independent original-Python diagnosis; leaves all evaluator source files unchanged."""
import json
from pathlib import Path
import sys
import time
from benchmark_submissions import ROOT, read, save
sys.path.insert(0,str(ROOT/'前辈代码_cpp/tests'))
import compare_original as reference
folder=Path(sys.argv[1]).resolve()
graph=read(ROOT/'hackathon_data'/f'{folder.name}.json')
for op in graph['ops']:
    if op['op'] in ('COPY_IN','COPY_OUT') and op['cycles']==-1: op['cycles']=1
start=time.perf_counter()
try:
    result=reference.evaluate_scene_b(graph,read(folder/'plan.json'),60,{'L1':1572864,'UB':131072},1000)
    actual=read(folder/'evaluate.stdout.txt')
    result=dict(valid=True,makespan=result['makespan'],full_result_equal=reference.difference(reference.normalized(result),actual) is None)
except Exception as error:
    result=dict(valid=False,error=str(error)[:6000],locals=[])
    tb=error.__traceback__
    while tb:
        frame=tb.tb_frame
        if frame.f_code.co_name in ('step3_simulation','raise_deadlock'):
            state=frame.f_locals
            index=state.get('next_allocation_rank',0)
            order=state.get('allocation_order',[])
            front=order[index] if index<len(order) else None
            tensors=state.get('tensor_by_id',{})
            detail=dict(function=frame.f_code.co_name,front=front,
                        allocation_index=index, memory_used=state.get('memory_used'),
                        resident=[tensors.get(t,{'id':t}) for t in state.get('resident_tensors',[])],
                        front_op=state.get('op_by_id',{}).get(front),
                        front_outputs=state.get('out_tids',{}).get(front))
            result['locals'].append(detail)
        tb=tb.tb_next
result['seconds']=time.perf_counter()-start
save(folder/'python_reference.json',result)
print(json.dumps({k:v for k,v in result.items() if k!='locals'},ensure_ascii=True),flush=True)
