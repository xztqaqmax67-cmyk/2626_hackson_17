"""Windows process resource measurement for a submission; external test utility."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import subprocess
import time
from benchmark_submissions import ROOT, save

class Entry(C.Structure):
    _fields_=[('size',W.DWORD),('usage',W.DWORD),('pid',W.DWORD),('heap',C.c_size_t),
              ('module',W.DWORD),('threads',W.DWORD),('parent',W.DWORD),('priority',W.LONG),
              ('flags',W.DWORD),('name',W.WCHAR*260)]
class Memory(C.Structure):
    _fields_=[('cb',W.DWORD),('faults',W.DWORD),('peak_ws',C.c_size_t),('ws',C.c_size_t),
              ('peak_paged',C.c_size_t),('paged',C.c_size_t),('peak_nonpaged',C.c_size_t),
              ('nonpaged',C.c_size_t),('pagefile',C.c_size_t),('peak_pagefile',C.c_size_t),('private',C.c_size_t)]
K=C.WinDLL('kernel32',use_last_error=True)
P=C.WinDLL('psapi',use_last_error=True)
K.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD]
K.CreateToolhelp32Snapshot.restype=W.HANDLE
K.Process32FirstW.argtypes=[W.HANDLE,C.POINTER(Entry)]
K.Process32NextW.argtypes=[W.HANDLE,C.POINTER(Entry)]
K.CloseHandle.argtypes=[W.HANDLE]
P.GetProcessMemoryInfo.argtypes=[W.HANDLE,C.POINTER(Memory),W.DWORD]

def threads(pid):
    snap=K.CreateToolhelp32Snapshot(2,0)
    if snap==C.c_void_p(-1).value: return 0,[]
    item=Entry();item.size=C.sizeof(item)
    result=0;children=[]
    try:
        ok=K.Process32FirstW(snap,C.byref(item))
        while ok:
            if item.pid==pid: result=item.threads
            if item.parent==pid and item.name.lower()!='conhost.exe': children.append(item.name)
            ok=K.Process32NextW(snap,C.byref(item))
    finally:K.CloseHandle(snap)
    return result,children

parser=argparse.ArgumentParser()
parser.add_argument('variant',choices=['old','new'])
parser.add_argument('--case',default='case_05')
args=parser.parse_args()
project='hackathon_v3' if args.variant=='old' else '前辈代码_cpp'
folder=ROOT/'output/submission_review_20261030/resources'/args.variant
folder.mkdir(parents=True,exist_ok=True)
exe=ROOT/project/'build/submission_msvc/bin/npu_sched.exe'
cmd=[str(exe),str(ROOT/'hackathon_data'/f'{args.case}.json'),'-n','5','-o',str(folder/'plan.json')]
start=time.perf_counter()
with (folder/'stdout.txt').open('wb') as out,(folder/'stderr.txt').open('wb') as err:
    proc=subprocess.Popen(cmd,stdin=subprocess.DEVNULL,stdout=out,stderr=err)
    result=dict(variant=args.variant,case=args.case,cores=5,peak_working_set_bytes=0,
                peak_pagefile_bytes=0,max_observed_threads=0,child_executables=[],samples=0)
    while True:
        m=Memory();m.cb=C.sizeof(m)
        if P.GetProcessMemoryInfo(W.HANDLE(int(proc._handle)),C.byref(m),C.sizeof(m)):
            result['peak_working_set_bytes']=max(result['peak_working_set_bytes'],m.peak_ws)
            result['peak_pagefile_bytes']=max(result['peak_pagefile_bytes'],m.peak_pagefile)
        count,children=threads(proc.pid)
        result['max_observed_threads']=max(result['max_observed_threads'],count)
        result['child_executables']=sorted(set(result['child_executables']+children))
        result['samples']+=1
        if proc.poll() is not None:break
        if time.perf_counter()-start>=90:
            proc.kill();proc.wait();result['timeout']=True;break
        time.sleep(.05)
result.update(returncode=proc.returncode,seconds=time.perf_counter()-start,
              note='OS peak counters queried during/after process lifetime; threads and children sampled every 50 ms; conhost excluded')
save(folder/'resources.json',result)
print({k:v for k,v in result.items() if k!='note'},flush=True)
