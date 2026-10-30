"""Paired serial/parallel generator timing; no evaluator or resource polling while timing."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


def use_performance_cores():
    """Restrict this benchmark process and its children, not the user's other apps."""
    import ctypes
    import struct
    assert os.name == 'nt' and ctypes.sizeof(ctypes.c_void_p) == 8
    kernel = ctypes.WinDLL('kernel32',use_last_error=True)
    query = kernel.GetLogicalProcessorInformationEx
    query.argtypes = [ctypes.c_int,ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
    query.restype = ctypes.c_int
    size = ctypes.c_ulong()
    query(0,None,ctypes.byref(size))
    buffer = ctypes.create_string_buffer(size.value)
    assert query(0,buffer,ctypes.byref(size)),ctypes.get_last_error()
    raw = buffer.raw[:size.value]
    position, masks = 0, {}
    while position < len(raw):
        relation,length = struct.unpack_from('<II',raw,position)
        assert relation == 0 and length >= 48
        efficiency = raw[position+9]
        groups = struct.unpack_from('<H',raw,position+30)[0]
        for index in range(groups):
            mask,group = struct.unpack_from('<QH',raw,position+32+16*index)
            assert group == 0, 'this diagnostic supports a single processor group'
            masks[efficiency] = masks.get(efficiency,0) | mask
        position += length
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    handle = kernel.GetCurrentProcess()
    get = kernel.GetProcessAffinityMask
    get.argtypes = [ctypes.c_void_p,ctypes.POINTER(ctypes.c_size_t),ctypes.POINTER(ctypes.c_size_t)]
    original,system = ctypes.c_size_t(),ctypes.c_size_t()
    assert get(handle,ctypes.byref(original),ctypes.byref(system))
    chosen = masks[max(masks)] & original.value
    assert chosen.bit_count() >= 4
    set_mask = kernel.SetProcessAffinityMask
    set_mask.argtypes = [ctypes.c_void_p,ctypes.c_size_t]
    assert set_mask(handle,chosen),ctypes.get_last_error()
    return dict(mode='same highest-efficiency-class core set for both variants',
                original_process_mask=hex(original.value), selected_mask=hex(chosen),
                selected_logical_processors=[n for n in range(64) if chosen>>n&1],
                detected_efficiency_class_masks={str(key):hex(value) for key,value in masks.items()},
                changed_only_this_benchmark_process_and_children=True)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def aggregate(rows):
    serial = sum(row['serial_median_seconds'] for row in rows)
    parallel = sum(row['parallel_median_seconds'] for row in rows)
    return dict(configurations=len(rows), serial_sum_medians_seconds=serial,
                parallel_sum_medians_seconds=parallel, speedup=serial/parallel,
                saved_seconds=serial-parallel, saved_percent=100*(1-parallel/serial),
                geometric_mean_case_speedup=math.exp(statistics.mean(math.log(row['speedup']) for row in rows)))


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    root = Path(__file__).resolve().parents[1]
    v3 = root.parent/'dag_chain_solver_v3'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=root/'runs/thread_benchmark_20261030')
    parser.add_argument('--performance-cores',action='store_true',
                        help='Diagnostic: both binaries inherit the same higher-performance Windows core set')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    assert not (out/'benchmark.json').exists(), 'use a fresh output directory for a fresh benchmark'
    affinity = use_performance_cores() if args.performance_cores else dict(mode='unchanged')
    executables = dict(serial=root/'build/bin/npu_sched.exe', parallel=v3/'build/bin/npu_sched.exe')
    data = v3/'references/hackathon_20261030/data'
    official = json.loads((v3/'reports/official_evaluation_v3_1_20261030.json').read_text(encoding='utf-8'))
    assert sha(executables['parallel']) == official['generator_executable_sha256']
    for name in ['model.hpp', 'chain.hpp']:
        assert (root/'src'/name).read_bytes() == (v3/'src'/name).read_bytes()
    def search_prefix(folder):
        text = (folder/'src/chain.cpp').read_text(encoding='utf-8').split('Result solve(')[0]
        return text.replace('#include "parallel_jobs.hpp"\n', '').replace('#include <optional>\n', '')
    assert search_prefix(root) == search_prefix(v3), 'search/estimator logic differs'
    assert (root/'src/main.cpp').read_text(encoding='utf-8').replace('0.2.1','0.3.1') == \
           (v3/'src/main.cpp').read_text(encoding='utf-8'), 'generator I/O differs'
    versions = {name: subprocess.check_output([str(path),'--version'], text=True).strip()
                for name, path in executables.items()}
    hardware = dict(os=platform.platform(), logical_processors=os.cpu_count(), processor=platform.processor())
    if os.name == 'nt':
        import winreg
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
            hardware['cpu_name'] = winreg.QueryValueEx(key, 'ProcessorNameString')[0]
        power = subprocess.run(['powercfg','/getactivescheme'], capture_output=True)
        hardware['power_scheme'] = power.stdout.decode(errors='replace').strip()
    report = dict(started_utc=datetime.now(timezone.utc).isoformat(), completed=False,
                  method='one warmup each, four paired rounds AB/BA/BA/AB; one process at a time; no resource polling',
                  units='seconds of complete generator process, including input/output',
                  hardware=hardware, affinity=affinity,
                  compiler='MSVC 19.44 x64 Release, same compiler and optimization flags',
                  versions=versions, executable_sha256={name:sha(path) for name,path in executables.items()},
                  shared_model_sha256=sha(root/'src/model.hpp'),
                  shared_search_prefix_sha256=hashlib.sha256(search_prefix(root).encode()).hexdigest(),
                  source_sha256={label:{p.relative_to(folder).as_posix():sha(p)
                                for p in sorted((folder/'src').glob('*')) if p.is_file()}
                                for label,folder in [('serial',root),('parallel',v3)]},
                  rounds=4, warmups_per_version_per_configuration=1, rows=[])
    def save():
        temporary = out/'benchmark.json.tmp'
        temporary.write_text(json.dumps(report,indent=2),encoding='utf-8')
        temporary.replace(out/'benchmark.json')
    save()
    def run(version, graph, n, target, digest):
        command = [str(executables[version]), str(graph), '-n', str(n), '-o', str(target)]
        start = time.perf_counter_ns()
        process = subprocess.run(command, capture_output=True, timeout=90)
        elapsed = (time.perf_counter_ns()-start)/1e9
        assert process.returncode == 0, (command,process.returncode,process.stderr)
        assert not process.stdout.strip(), (command,process.stdout)
        assert sha(target) == digest, (graph.name,n,version,'output differs from official v3 plan')
        return elapsed
    for index, expected in enumerate(sorted(official['rows'],key=lambda row:(row['case'],row['cores']))):
        case, n = expected['case'], expected['cores']
        graph = data/f'{case}.json'
        assert sha(graph) == expected['input_sha256']
        folder = out/f'{case}_n{n}'
        folder.mkdir(exist_ok=True)
        targets = {version:folder/f'{version}.json' for version in executables}
        warm_order = ['serial','parallel'] if index%2 == 0 else ['parallel','serial']
        warmups = {version:run(version,graph,n,targets[version],expected['plan_sha256']) for version in warm_order}
        measured = []
        for order in [('serial','parallel'),('parallel','serial'),('parallel','serial'),('serial','parallel')]:
            pair = dict(order=list(order))
            for version in order:
                pair[version] = run(version,graph,n,targets[version],expected['plan_sha256'])
            measured.append(pair)
        serial = statistics.median(pair['serial'] for pair in measured)
        parallel = statistics.median(pair['parallel'] for pair in measured)
        row = dict(case=case,cores=n,input_sha256=expected['input_sha256'],plan_sha256=expected['plan_sha256'],
                   outputs_identical_to_official_v3=True,warmups_seconds=warmups,measured_pairs=measured,
                   serial_median_seconds=serial,parallel_median_seconds=parallel,speedup=serial/parallel,
                   paired_ratio_median=statistics.median(pair['serial']/pair['parallel'] for pair in measured),
                   serial_range_seconds=[min(pair['serial'] for pair in measured),max(pair['serial'] for pair in measured)],
                   parallel_range_seconds=[min(pair['parallel'] for pair in measured),max(pair['parallel'] for pair in measured)])
        report['rows'].append(row)
        save()
        print(json.dumps(dict(case=case,cores=n,serial_seconds=round(serial,4),parallel_seconds=round(parallel,4),
                              speedup=round(serial/parallel,4))),flush=True)
    report['overall'] = aggregate(report['rows'])
    report['by_cores'] = {str(n):aggregate([row for row in report['rows'] if row['cores']==n]) for n in range(2,6)}
    report['completed'] = True
    report['finished_utc'] = datetime.now(timezone.utc).isoformat()
    save()
    print(json.dumps(dict(overall=report['overall'],by_cores=report['by_cores']),indent=2),flush=True)


if __name__ == '__main__':
    main()
