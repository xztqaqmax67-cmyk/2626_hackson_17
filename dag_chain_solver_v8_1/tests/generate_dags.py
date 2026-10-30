"""Deterministic development fixtures; never part of solver or submission."""
import argparse
import json
from pathlib import Path
import random

p=argparse.ArgumentParser();p.add_argument("--out",type=Path,required=True);a=p.parse_args()
a.out.mkdir(parents=True,exist_ok=True)
for seed in range(20):
    rng=random.Random(seed);n=24
    opids=rng.sample(range(100,1000),n)
    ops=[dict(id=v,op="COMPUTE",pipe=rng.choice(["PIPE_M","PIPE_V","PIPE_FIX","PIPE_MTE2","PIPE_MTE3"]),cycles=rng.randint(1,1000)) for v in opids]
    tensors=[dict(id=2000+i,pos="UB",size=rng.choice([1024,4096,16384])) for i in range(n)]
    edges=[dict(source=opids[i],target=2000+i) for i in range(n)]
    used=set()
    for i in range(1,n):
        for j in rng.sample(range(i),rng.randint(1,min(3,i))):
            edges.append(dict(source=2000+j,target=opids[i]));used.add(j)
    # Explicit boundary input/output COPY nodes, including -1 placeholders.
    tensors.extend([dict(id=3000,pos="DDR",size=1024),dict(id=3001,pos="UB",size=1024)])
    ops.append(dict(id=4000,op="COPY_IN",pipe="PIPE_MTE2",cycles=-1))
    edges.extend([dict(source=3000,target=4000),dict(source=4000,target=3001),dict(source=3001,target=opids[0])])
    for i in range(n):
        if i not in used:
            tensors.append(dict(id=5000+i,pos="DDR",size=tensors[i]["size"]))
            ops.append(dict(id=6000+i,op="COPY_OUT",pipe="PIPE_MTE3",cycles=-1))
            edges.extend([dict(source=2000+i,target=6000+i),dict(source=6000+i,target=5000+i)])
    if seed%3==0:
        tensors.append(dict(id=7000,pos="UB",size=16384))
        edges.extend([dict(source=opids[0],target=7000),dict(source=opids[1],target=7000),dict(source=7000,target=opids[-1])])
    rng.shuffle(ops);rng.shuffle(tensors);rng.shuffle(edges)
    (a.out/f"random_{seed:02d}.json").write_text(json.dumps(dict(ops=ops,tensors=tensors,edges=edges)),encoding="utf-8")

