"""Check fresh builds against the exact plans already externally evaluated."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import os
import ctypes

class MemoryCounters(ctypes.Structure):
    _fields_=[("cb",ctypes.c_ulong),("faults",ctypes.c_ulong),
              *[(name,ctypes.c_size_t) for name in ("peak_working","working","peak_paged",
                "paged","peak_nonpaged","nonpaged","pagefile","peak_pagefile")]]

def memory(process):
    if os.name!="nt":return 0
    counters=MemoryCounters();counters.cb=ctypes.sizeof(counters)
    ok=ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(process._handle)),
                                              ctypes.byref(counters),counters.cb)
    return counters.peak_working if ok else 0

p=argparse.ArgumentParser();p.add_argument("--solver",type=Path,required=True)
p.add_argument("--data",type=Path,required=True);p.add_argument("--plans",type=Path,required=True)
p.add_argument("--out",type=Path,required=True);a=p.parse_args();a.out.mkdir(parents=True,exist_ok=True)
report=[]
for file in sorted(a.plans.glob("*_chain.json")):
    case,core,_=file.stem.rsplit("_",2);cores=int(core[1:])
    target=a.out/file.name
    started=time.perf_counter()
    process=subprocess.Popen([str(a.solver.resolve()),str((a.data/(case+".json")).resolve()),
                              "-n",str(cores),"-o",str(target.resolve())],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    peak=0
    while process.poll() is None:
        peak=max(peak,memory(process))
        if time.perf_counter()-started>90:
            process.kill();raise AssertionError("90 second limit exceeded")
        time.sleep(.02)
    peak=max(peak,memory(process));stdout,stderr=process.communicate()
    assert process.returncode==0,(file.name,stderr)
    identical=target.read_bytes()==file.read_bytes()
    row=dict(file=file.name,identical=identical,seconds=time.perf_counter()-started,
             sampled_peak_working_set_bytes=peak)
    report.append(row)
    assert identical,(file.name,"different from externally validated plan")
a.out.joinpath("comparison.json").write_text(json.dumps(dict(
    executable_sha256=hashlib.sha256(a.solver.read_bytes()).hexdigest(),plans=report),indent=2),encoding="utf-8")
print(f"{len(report)} plans match externally evaluated plans byte for byte")
