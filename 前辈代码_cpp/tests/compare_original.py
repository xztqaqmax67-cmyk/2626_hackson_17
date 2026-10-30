"""使用用户提供的 hackathon.zip 官方 Python 作对照；C++ 程序本身不依赖 Python。"""
import importlib
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from official_reference import *
from schedule_step2 import _build_extended_graph
sys.path.insert(0, str(ROOT / '前辈代码'))
from problem2 import core, bootstrap, structure, pipeline

def compare(action, graph, expected, **kwargs):
    request = dict(action=action, graph=graph, capacity=kwargs.pop('capacity', CAPACITY), **kwargs)
    run = subprocess.run([str(RUNNER)], input=json.dumps(request), text=True,
                         encoding='utf-8', capture_output=True)
    actual = json.loads(run.stdout)
    error = difference(normalized(expected), actual)
    if error:
        directory = BUILD / 'parity_failure'
        directory.mkdir(exist_ok=True)
        for name, value in [('request', request), ('expected', expected), ('actual', actual)]:
            (directory / f'{name}.json').write_text(json.dumps(normalized(value), indent=2), encoding='utf-8')
        raise AssertionError(f'{action}: {error}')
    print(f'PASS {action}', flush=True)

def compare_rejection(action, graph, **kwargs):
    request = dict(action=action, graph=graph, capacity=CAPACITY, **kwargs)
    run = subprocess.run([str(RUNNER)], input=json.dumps(request), text=True,
                         encoding='utf-8', capture_output=True, timeout=90)
    assert run.returncode != 0 and 'error' in json.loads(run.stdout), (action, run.stdout)
    print(f'PASS {action} rejects unsupported input (as original)', flush=True)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('count', nargs='?', type=int, default=10)
    parser.add_argument('--start', type=int, default=0)
    args = parser.parse_args()
    paths = sorted((ROOT / 'hackathon_data').glob('case_*.json'))
    paths = paths[args.start:args.start+args.count]
    for path in paths:
        print(path.name, flush=True)
        graph = json.loads(path.read_text(encoding='utf-8'))
        seq = step1_schedule(graph)
        compare('step1', graph, seq)
        result2 = step2_spill_insertion(graph, seq, CAPACITY)
        compare('step2', graph, result2, sequence=seq)
        extended = _build_extended_graph(graph, result2)
        try:
            result3 = step3_simulation(extended, CAPACITY, 60)
        except (KeyError, ValueError, RuntimeError) as error:
            print(f'Original Step3 rejects input: {type(error).__name__}: {error}', flush=True)
            compare_rejection('step3', extended)
        else:
            compare('step3', extended, result3)
        try:
            validate_graph(graph)
        except EvaluationValidationError as error:
            print(f'Original evaluator rejects graph: {error}', flush=True)
            compare_rejection('generate', graph)
            continue
        for seed in (0, 7, 4294967297):
            plan = generate_multicore_plan(graph, num_cores=3, seed=seed)
            compare('generate', graph, plan, cores=3, seed=seed)
        info = core.GraphInfo(graph)
        settings = {'capacity': CAPACITY, 'bandwidth': 60, 'cross_core_copy_delay': 500}
        native = core.construct(info, 3, settings=settings)
        seeds = list(structure.seed_plans(info, 3, settings)) + list(pipeline.seed_plans(info, 3, settings))
        compare('core', graph, dict(id_order=info.id_order, locality=info.topological_order(),
            critical=info.topological_order('critical'), atoms=info.atoms,
            bootstrap=bootstrap.first_plan(info, 3).to_dict(), construct=native.to_dict(),
            fingerprint=native.fingerprint(), route=core.route_bytes(info, native, details=True),
            sketch=core.replay_sketch(info, native, settings, details=True),
            seeds=[(label, plan.to_dict()) for label, plan in seeds]))
        # 全图放在单核：用于验证合并 Task 后重建 I/O 的路径。
        plan = {'node_to_subgraph': {str(op['id']): 0 for op in graph['ops']
                                    if op['op'] not in ('COPY_IN', 'COPY_OUT')},
                'core_schedules': [[0], []]}
        expected = evaluate_scene_b(graph, plan, 60, CAPACITY, 500)
        compare('scene_b', graph, expected, plan=plan)

if __name__ == '__main__':
    main()
