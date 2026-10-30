"""Independent sampled check against the supplied Python Scene B implementation.

Only the hardware Pipe tuple and COPY placeholder values are adapted in memory.
"""
import argparse
import json
from pathlib import Path
import sys
import time

p=argparse.ArgumentParser()
p.add_argument("--reference",type=Path,required=True)
p.add_argument("--data",type=Path,required=True)
p.add_argument("--plans",type=Path,required=True)
p.add_argument("--cases",nargs="+",default=["case_01","case_07"])
p.add_argument("--out",type=Path,required=True)
a=p.parse_args();sys.dont_write_bytecode=True
sys.path.insert(0,str(a.reference.resolve()))
from multicore_cut_evaluate_problem_2 import evaluate_scene_b
for module in list(sys.modules.values()):
    pipes=getattr(module,"PIPES",None)
    if isinstance(pipes,tuple) and "PIPE_MTE2" in pipes and "PIPE_FIX" not in pipes:
        module.PIPES=(*pipes,"PIPE_FIX")
report=json.loads((a.plans/"report.json").read_text(encoding="utf-8"))
results=[]
for case in a.cases:
    graph=json.loads((a.data/(case+".json")).read_text(encoding="utf-8"))
    for op in graph["ops"]:
        if op["op"] in {"COPY_IN","COPY_OUT"} and op["cycles"]==-1:op["cycles"]=1
    plan=json.loads((a.plans/(case+"_n4_chain.json")).read_text(encoding="utf-8"))
    start=time.perf_counter()
    result=evaluate_scene_b(graph,plan,60,{"L1":1572864,"UB":131072},1000)
    expected=next(r for r in report["rows"] if r["case"]==case and r["cores"]==4 and r["strategy"]=="chain")
    assert result["makespan"]==expected["makespan"]
    assert result["data_movement_bytes"]==expected["data_movement_bytes"]
    row=dict(case=case,cores=4,makespan=result["makespan"],movement=result["data_movement_bytes"],
             matched=True,evaluation_seconds=time.perf_counter()-start)
    results.append(row);print(json.dumps(row),flush=True)
    a.out.parent.mkdir(parents=True,exist_ok=True)
    a.out.write_text(json.dumps(results,indent=2),encoding="utf-8")
