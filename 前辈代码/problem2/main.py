#!/usr/bin/env python3
"""附件 B 单图入口：读取外部计算图，限时搜索并输出标准多核方案。"""
from __future__ import annotations

import argparse
import importlib
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

from submission_io import safe_message, install_error_handler, load_local_package
install_error_handler()

PROBLEM = 2
PACKAGE = Path(__file__).resolve().parent
PACKAGE_NAME = load_local_package(PACKAGE, '_submission_problem2')
OFFICIAL_DIR = PACKAGE / 'official_code'
sys.path.insert(0, str(OFFICIAL_DIR))
ENV = {name: '1' for name in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS',
       'MKL_NUM_THREADS', 'NUMEXPR_NUM_THREADS', 'VECLIB_MAXIMUM_THREADS', 'BLIS_NUM_THREADS')}
ENV.update(PYTHONDONTWRITEBYTECODE='1', PYTHONHASHSEED='0')


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def worker(args):
    # Validate JSON with the same duplicate-key policy and schema as the official CLI.
    from contest_io import _read_json
    from evaluation_validation import validate_graph, read_evaluation_config
    validate_graph(_read_json(args.graph))
    read_evaluation_config(str(args.config))
    remaining = args._deadline - time.monotonic()
    grace = max(0., args._hard_deadline - max(time.monotonic(), args._deadline))
    if remaining <= 0 and grace <= 0:
        raise TimeoutError('输入校验后剩余搜索时间不足，尚未产生完整方案')
    search = importlib.import_module(PACKAGE_NAME + '.search')
    record = search.solve(args.graph, args.cores, args.config, args.work_dir, max(.001, remaining),
                          inherited_checkpoint=args.inherit_checkpoint,
                          feasibility_grace=grace, compact=args.search_profile == 'compact',
                          export_results=False)
    return 0 if record.get('solution_available') else 1


def export_checkpoint(args, target):
    from contest_io import _read_json
    from stub_multicore_cut_and_schedule import derive_multicore_plan
    from evaluation_validation import validate_task_order
    checkpoint = args.work_dir / 'best_checkpoint.json'
    if not checkpoint.is_file():
        raise RuntimeError('时限内未得到经过官方评价的完整方案，未写出方案文件')
    payload = _read_json(checkpoint)
    plan = payload['plan']
    result = payload['result' if PROBLEM == 1 else 'official_result']
    if set(plan) != {'node_to_subgraph', 'core_schedules'}:
        raise ValueError('方案顶层字段不符合附录 B')
    if (len(plan['core_schedules']) != args.cores or result['num_cores'] != args.cores
            or result.get('scene', 'A') != ('A' if PROBLEM == 1 else 'B')
            or not math.isfinite(result['makespan']) or result['makespan'] < 0):
        raise ValueError('检查点的场景、核心数或目标值不一致')
    best = payload['best'] if PROBLEM == 1 else payload
    if best['makespan'] != result['makespan']:
        raise ValueError('检查点目标值与官方结果不一致')
    if any(not isinstance(k, str) or str(int(k)) != k for k in plan['node_to_subgraph']):
        raise ValueError('操作 ID 必须为十进制整数字符串')
    graph = _read_json(args.graph)
    validate_task_order(derive_multicore_plan(graph, plan))
    atomic_json(target, plan)
    return result


def stop_group(child):
    # Includes the scene-A evaluator subprocess, so no search survives its deadline.
    try:
        os.killpg(child.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    child.wait()


def run(args, target, started):
    # Reserve a small part of the same budget for checkpoint validation/export.
    deadline = started + args.time_limit - min(2., args.time_limit * .02)
    hard_deadline = deadline + args.feasibility_grace
    sys.path.insert(0, str(PACKAGE))
    from time_budget import wait_for_search
    command = [sys.executable, '-B', str(Path(__file__).resolve()), str(args.graph),
               '-n', str(args.cores), '--config', str(args.config),
               '--time-limit', str(args.time_limit), '--work-dir', str(args.work_dir),
               '--_deadline', str(deadline), '--_hard-deadline', str(hard_deadline),
               '--feasibility-grace', str(args.feasibility_grace),
               '--search-profile', args.search_profile]
    for flag in ('seed_index', 'inherit_checkpoint'):
        value = getattr(args, flag, None)
        if value is not None:
            command += ['--' + flag.replace('_', '-'), str(value)]
    timed_out = interrupted = extension_used = False
    def terminate(signum, frame):
        raise KeyboardInterrupt
    with (args.work_dir / 'search.log').open('w', encoding='utf-8') as log:
        child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True,
                                 env={**os.environ, **ENV})
        previous_term = signal.signal(signal.SIGTERM, terminate)
        try:
            _, timed_out, extension_used = wait_for_search(
                child, deadline, hard_deadline, args.work_dir / 'best_checkpoint.json')
            if timed_out:
                stop_group(child)
        except KeyboardInterrupt:
            interrupted = True
            stop_group(child)
        finally:
            if child.poll() is None:
                stop_group(child)
            signal.signal(signal.SIGTERM, previous_term)
    search_seconds = time.monotonic() - started
    try:
        result = export_checkpoint(args, target)
        if args.export_evaluation:
            artifacts = importlib.import_module(PACKAGE_NAME + '.artifacts')
            artifacts.export_evaluation(target.parent, args.graph.name, target.name, result)
    except Exception:
        tail = (args.work_dir / 'search.log').read_text(encoding='utf-8')[-2500:]
        if tail:
            print(safe_message(tail), file=sys.stderr)
        raise
    abnormal = bool(child.returncode and not timed_out and not interrupted)
    metadata = dict(problem=PROBLEM, input=args.graph.name, config=args.config.name,
                    output=target.name, cores=args.cores, makespan=result['makespan'],
                    data_movement_bytes=result['data_movement_bytes'],
                    search_seconds=search_seconds, total_seconds=time.monotonic() - started,
                    time_limit=args.time_limit, timed_out=timed_out,
                    feasibility_grace_seconds=args.feasibility_grace,
                    feasibility_extension_used=extension_used, search_profile=args.search_profile,
                    worker_returncode=child.returncode,
                    status='interrupted' if interrupted else 'complete_with_error' if abnormal else 'complete')
    atomic_json(args.work_dir / 'submission_report.json', metadata)
    print(json.dumps(metadata, ensure_ascii=False, allow_nan=False))
    if abnormal:
        print(safe_message((args.work_dir / 'search.log').read_text(encoding='utf-8')[-2500:]), file=sys.stderr)
    return 130 if interrupted else 1 if abnormal else 0


def main():
    started = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('graph', type=Path, help='UTF-8 计算图 JSON；名称不限于 case_数字')
    parser.add_argument('-n', '--cores', type=int, required=True, help='调度目标核心数，1 至 5')
    parser.add_argument('--config', type=Path, help='默认读取计算图同目录的 config.txt')
    destinations = parser.add_mutually_exclusive_group()
    destinations.add_argument('-o', '--output', type=Path, help='方案 JSON 路径；默认 <图目录>/<图名>_multicore_res.json')
    destinations.add_argument('--output-dir', type=Path, help='在此目录输出 <图名>_multicore_res.json')
    parser.add_argument('--time-limit', '--budget', type=float, default=None, help='目标墙钟秒数；2–5 核默认 60/120/180/240，单核 600')
    parser.add_argument('--feasibility-grace', type=float, default=60., help='仅未得有效解时允许的延时秒数，最大 120')
    parser.add_argument('--search-profile', choices=('compact', 'full'), default='compact')
    parser.add_argument('--export-evaluation', action='store_true', help='同时导出附录 B 的结果、时间线和日志，不重复评价')
    parser.add_argument('--work-dir', type=Path, help='可选：保存检查点与日志的空目录；默认使用临时目录并自动清理')
    if PROBLEM == 1:
        parser.add_argument('--seed-index', type=Path, help='可选：显式指定经过来源校验的历史索引')
    else:
        parser.add_argument('--inherit-checkpoint', type=Path, help='可选：重评同一输入的低核检查点，保留不退化初解')
    parser.add_argument('--_deadline', type=float, help=argparse.SUPPRESS)
    parser.add_argument('--_hard-deadline', type=float, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.time_limit is None:
        args.time_limit = 600. if args.cores == 1 else 60. * (args.cores - 1)
    if not math.isfinite(args.feasibility_grace) or not 0 <= args.feasibility_grace <= 120:
        parser.error('首解延时必须在 [0, 120] 秒内')
    if args.cores == 1:
        args.feasibility_grace = 0.
    if not 1 <= args.cores <= 5 or not math.isfinite(args.time_limit) or not 1 < args.time_limit <= 1200:
        parser.error('目标核心数必须为 1 至 5，总搜索时限必须在 (1, 1200] 秒内')
    args.graph = args.graph.resolve()
    args.config = (args.config or args.graph.parent / 'config.txt').resolve()
    if not args.graph.is_file() or not args.config.is_file():
        parser.error('计算图或配置文件不存在；可使用 --config 指定外部配置')
    for flag in ('seed_index', 'inherit_checkpoint'):
        if getattr(args, flag, None) is not None:
            setattr(args, flag, getattr(args, flag).resolve())
    if args._deadline is not None:
        return worker(args)
    target = (args.output or (args.output_dir or args.graph.parent) /
              (args.graph.stem + '_multicore_res.json')).resolve()
    if target in (args.graph, args.config) or target.is_relative_to(PACKAGE):
        parser.error('方案不能覆盖输入/配置文件，也不能写入附件源码目录')
    if args.work_dir:
        args.work_dir = args.work_dir.resolve()
        if args.work_dir.is_relative_to(PACKAGE) or (args.work_dir.exists() and
                (not args.work_dir.is_dir() or any(args.work_dir.iterdir()))):
            parser.error('--work-dir 必须是源码目录外的空目录，避免混用旧检查点')
        if target.is_relative_to(args.work_dir):
            parser.error('最终方案与工作目录分开保存，请调整 --output 或 --work-dir')
        args.work_dir.mkdir(parents=True, exist_ok=True)
        return run(args, target, started)
    with tempfile.TemporaryDirectory(prefix=f'gmcm_problem{PROBLEM}_') as directory:
        args.work_dir = Path(directory)
        return run(args, target, started)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(f'[SOLVER ERROR] {safe_message(error)}', file=sys.stderr)
        raise SystemExit(1)
