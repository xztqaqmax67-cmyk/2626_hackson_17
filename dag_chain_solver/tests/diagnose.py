"""Inspect an external evaluator failure without modifying the evaluator."""
import argparse
import json
from pathlib import Path
import subprocess
from collections import defaultdict, deque

p=argparse.ArgumentParser()
p.add_argument("evaluator",type=Path);p.add_argument("graph",type=Path);p.add_argument("plan",type=Path)
a=p.parse_args()
def call(action,graph,**extra):
    request=dict(action=action,graph=graph,capacity={"L1":1572864,"UB":131072},bandwidth=60,delay=1000,**extra)
    r=subprocess.run([str(a.evaluator.resolve())],input=json.dumps(request),text=True,capture_output=True,timeout=180)
    return json.loads(r.stdout)
raw=json.loads(a.graph.read_text());plan=json.loads(a.plan.read_text())
boundary=call("boundary",raw,plan=plan)
for core,task in boundary["tasks"].items():
    graph=task["graph"];seq=call("step1",graph)
    priority={b:i for i,b in enumerate(task["subgraph_order"])}
    seq.sort(key=lambda v:priority[task["op_subgraph"][str(v)]])
    ext=call("step2",graph,sequence=seq)
    if "error" in ext:
        print(core,ext);continue
    producers=defaultdict(set);ids={o["id"] for o in graph["ops"]}
    for e in graph["edges"]:
        if e["source"] in ids and e["target"] not in ids:producers[e["target"]].add(e["source"])
    multi=[s for s in ext["spill_records"] if len(producers[s["logical_tid"]])>1]
    expanded=dict(ops=graph["ops"]+ext["new_ops"],tensors=graph["tensors"]+ext["new_tensors"],edges=ext["ext_edges"],seq_ext=ext["seq_ext"])
    scored=call("step3",expanded)
    print(json.dumps(dict(core=core,spills=len(ext["spill_records"]),multiwriter_spills=len(multi),
                         example=multi[:1],error=scored.get("error"),makespan=scored.get("makespan"))))
