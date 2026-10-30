"""Exercise the submission CLI in a directory containing Unicode and spaces."""
import argparse
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser()
p.add_argument("--solver",type=Path,required=True)
p.add_argument("--out",type=Path,required=True)
a=p.parse_args();exe=a.solver.resolve();root=a.out.resolve()/"中文 paths"
root.mkdir(parents=True,exist_ok=True)
graph=dict(tensors=[dict(id=1,pos="DDR",size=16),dict(id=2,pos="UB",size=16),
                    dict(id=3,pos="UB",size=16),dict(id=4,pos="DDR",size=16)],
           ops=[dict(id=10,op="COPY_IN",pipe="PIPE_MTE2",cycles=-1),
                dict(id=11,op="ADD",pipe="PIPE_FIX",cycles=4),
                dict(id=12,op="COPY_OUT",pipe="PIPE_MTE3",cycles=-1)],
           edges=[dict(source=s,target=t) for s,t in [(1,10),(10,2),(2,11),(11,3),(3,12),(12,4)]])
source=root/"最小 input.json";source.write_text(json.dumps(graph),encoding="utf-8")
def run(args,code):
    r=subprocess.run([str(exe),*map(str,args)],cwd=root,encoding="utf-8",errors="replace",capture_output=True,timeout=30)
    assert r.returncode==code,(args,r.returncode,r.stdout,r.stderr)
    return r
assert "0.8.1" in run(["--version"],0).stdout
target=root/"nested"/"方案.json"
run([source,"--cores=2","--output="+str(target)],0)
first=target.read_bytes();run([source,"-n","2","-o",target],0)
assert first==target.read_bytes(), "default fixed search must repeat identically"
plan=json.loads(first);assert set(plan)=={"node_to_subgraph","core_schedules"}
assert plan["node_to_subgraph"]=={"11":11} and len(plan["core_schedules"])==2
run([source],0);assert (root/"最小 input_multicore_res.json").exists()
for args in [[],[source,"--config","config.txt"],[source,"--time-limit=3"],
             [source,"-n","1"],[source,"-n","6"],[source,"-n"],
             [source,"-n","2","--cores=3"],[source,"-o",source],
             [source,"--output="],[source,"--unknown"]]:run(args,2)
run([root/"missing.json"],3)
bad=root/"bad.json";bad.write_text('{"tensors":[]',encoding="utf-8");run([bad],3)
blocked=root/"blocked";blocked.write_text("file, not directory",encoding="utf-8")
run([source,"-o",blocked/"out.json"],4)
large=json.loads(json.dumps(graph));large["tensors"][1]["size"]=131073
big=root/"too_big.json";big.write_text(json.dumps(large),encoding="utf-8")
before_failure=target.read_bytes()
run([big,"-o",target],5);assert target.read_bytes()==before_failure
assert json.loads(source.read_text(encoding="utf-8"))==graph
print("CLI contract checks passed: fixed options, exit codes, Unicode/spaces, input/output preservation; default fixed search repeats identically")

