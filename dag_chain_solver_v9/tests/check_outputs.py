"""Generate and compare JSON only. This script never invokes an evaluator."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import tempfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sample_resources(process):
    if os.name != "nt":
        return 0, 0
    from ctypes import wintypes
    class MemoryCounters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("faults", wintypes.DWORD),
                    *[(name, ctypes.c_size_t) for name in
                      ("peak_working", "working", "peak_paged", "paged", "peak_nonpaged",
                       "nonpaged", "pagefile", "peak_pagefile")]]
    counters = MemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(ctypes.c_void_p(int(process._handle)),
                                                ctypes.byref(counters), counters.cb)
    peak = counters.peak_working if ok else 0
    class ThreadEntry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                    ("id", wintypes.DWORD), ("owner", wintypes.DWORD),
                    ("priority", wintypes.LONG), ("delta", wintypes.LONG), ("flags", wintypes.DWORD)]
    kernel = ctypes.windll.kernel32
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(4, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        return peak, 0
    count = 0
    try:
        entry = ThreadEntry()
        entry.size = ctypes.sizeof(entry)
        more = kernel.Thread32First(snapshot, ctypes.byref(entry))
        while more:
            count += entry.owner == process.pid
            entry.size = ctypes.sizeof(entry)
            more = kernel.Thread32Next(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return peak, count


def generate(exe, graph, cores, target, observe=False):
    command = [str(exe.resolve()), str(graph.resolve()), "-n", str(cores), "-o", str(target.resolve())]
    start = time.perf_counter()
    if not observe:
        process = subprocess.run(command, capture_output=True, timeout=90)
        peak = threads = 0
        stdout, stderr = process.stdout, process.stderr
    else:
        # File-backed capture prevents verbose diagnostic lines filling a pipe
        # while the resource-observation loop waits for process completion.
        with tempfile.TemporaryFile() as output_file, tempfile.TemporaryFile() as error_file:
            process = subprocess.Popen(command, stdout=output_file, stderr=error_file)
            peak = threads = 0
            try:
                while process.poll() is None:
                    memory, count = sample_resources(process)
                    peak, threads = max(peak, memory), max(threads, count)
                    if time.perf_counter()-start > 90:
                        raise TimeoutError("generation exceeded 90 seconds")
                    time.sleep(.05)
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
            output_file.seek(0); error_file.seek(0)
            stdout, stderr = output_file.read(), error_file.read()
    assert process.returncode == 0, stderr.decode("utf-8", errors="replace")
    assert not stdout.strip(), "submission generator must not print scores"
    timing = dict(seconds=time.perf_counter()-start, observed_peak_working_set_bytes=peak,
                  sampled_max_process_threads=threads)
    for name in ('feedback_stats', 'communication_stats'):
        prefix = name + ' '
        diagnostics = [line[len(prefix):] for line in stderr.decode('utf-8', errors='replace').splitlines()
                       if line.startswith(prefix)]
        assert len(diagnostics) <= 1, 'duplicate ' + name
        if diagnostics:
            timing[name] = json.loads(diagnostics[0])
    return timing


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--reference", type=Path, required=True)
    p.add_argument("--solver", type=Path, required=True)
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cores", type=int, nargs="+", default=[2,3,4,5])
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    graphs = sorted(a.data.glob("*.json"))
    assert graphs, "no input graphs"
    report = dict(scoring_performed=False, check="fresh JSON generation and byte equality only",
                  reference_executable_sha256=sha256(a.reference), solver_executable_sha256=sha256(a.solver), rows=[])
    for graph in graphs:
        raw = json.loads(graph.read_text(encoding="utf-8"))
        expected = {str(op["id"]) for op in raw["ops"] if op["op"] not in {"COPY_IN","COPY_OUT"}}
        for cores in a.cores:
            reference = a.out / f"{graph.stem}_n{cores}_reference.json"
            target = a.out / f"{graph.stem}_n{cores}_v3.json"
            old = generate(a.reference, graph, cores, reference)
            new = generate(a.solver, graph, cores, target, observe=True)
            first = target.read_bytes()
            assert first == reference.read_bytes(), (graph.name, cores, "reference output differs")
            generate(a.solver, graph, cores, target)
            assert first == target.read_bytes(), (graph.name, cores, "repeat output differs")
            plan = json.loads(first)
            assert set(plan) == {"node_to_subgraph", "core_schedules"}
            assert set(plan["node_to_subgraph"]) == expected
            assert len(plan["core_schedules"]) == cores
            scheduled = [node for core in plan["core_schedules"] for node in core]
            assert len(scheduled) == len(set(scheduled))
            assert set(scheduled) == set(plan["node_to_subgraph"].values())
            assert new["sampled_max_process_threads"] <= 10
            row = dict(case=graph.stem, cores=cores, input_sha256=sha256(graph),
                       matches_reference=True, repeat_identical=True, plan_sha256=hashlib.sha256(first).hexdigest(),
                       reference_generation_seconds=old["seconds"], generation=new)
            report["rows"].append(row)
            (a.out/"report.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
            print(json.dumps({k:row[k] for k in ["case","cores","matches_reference","repeat_identical"]}),flush=True)
    print(f"{len(report['rows'])} output checks passed; no evaluator invoked.")


if __name__ == "__main__":
    main()
