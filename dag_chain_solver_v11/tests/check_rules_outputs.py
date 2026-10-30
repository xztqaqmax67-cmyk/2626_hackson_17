"""Check generated/repeated structure; officially score any differing repeat."""
import argparse
from collections import defaultdict, deque
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from check_outputs import generate, sha256


def check_plan(graph, plan, cores):
    ops = {op['id'] for op in graph['ops']}
    eligible = {op['id'] for op in graph['ops'] if op['op'] not in {'COPY_IN', 'COPY_OUT'}}
    tensors = {t['id'] for t in graph['tensors']}
    assert set(plan) == {'node_to_subgraph', 'core_schedules'}
    mapping = plan['node_to_subgraph']
    assert set(mapping) == {str(v) for v in eligible}
    assert all(type(sg) is int and sg >= 0 for sg in mapping.values())
    schedules = plan['core_schedules']
    assert len(schedules) == cores
    assigned = [sg for row in schedules for sg in row]
    assert len(assigned) == len(set(assigned))
    assert set(assigned) == set(mapping.values())
    # This solver uses one computation per subgraph.
    assert all(mapping[str(v)] == v for v in eligible)
    core_by_op = {sg: c for c, row in enumerate(schedules) for sg in row}
    writers = defaultdict(set)
    adjacency = defaultdict(list)
    degree = {v: 0 for v in ops | tensors}
    for edge in graph['edges']:
        a, b = edge['source'], edge['target']
        adjacency[a].append(b)
        degree[b] += 1
        if a in eligible and b in tensors:
            writers[b].add(core_by_op[a])
    assert all(len(cs) <= 1 for cs in writers.values()), 'tensor writers span cores'
    # Check the full original graph (including COPY) plus submitted same-core
    # computation order. This does not model generated COPY/Pipe/memory edges.
    for row in schedules:
        for a, b in zip(row, row[1:]):
            adjacency[a].append(b)
            degree[b] += 1
    ready = deque(v for v, count in degree.items() if count == 0)
    visited = 0
    while ready:
        v = ready.popleft()
        visited += 1
        for w in adjacency[v]:
            degree[w] -= 1
            if degree[w] == 0:
                ready.append(w)
    assert visited == len(degree), 'original dependencies and core orders form a cycle'


def main():
    sys.stdout.reconfigure(encoding='utf-8')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--solver', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--previous-output-dir', type=Path)
    parser.add_argument('--cores', type=int, nargs='+', default=[2, 3, 4, 5], choices=range(2, 6))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    graphs = sorted(args.data.glob('case_*.json'))
    assert graphs, 'no input graphs'
    report = dict(version='0.11.1', scoring_performed=False,
                  check='generation, both structures, writer affinity, dependency order; differing repeats are officially scored',
                  repeat_identity_required=False,
                  solver_executable_sha256=sha256(args.solver), rows=[])
    saved = args.out/'report.json'
    if saved.exists():
        previous = json.loads(saved.read_text(encoding='utf-8'))
        assert previous['solver_executable_sha256'] == report['solver_executable_sha256'], \
            'executable changed: use a fresh output directory'
        report['rows'] = previous['rows']
    completed = {(row['case'], row['cores']): row for row in report['rows']}
    for path in graphs:
        graph = json.loads(path.read_text(encoding='utf-8-sig'))
        for cores in args.cores:
            target = args.out / f'{path.stem}_n{cores}_v5.json'
            if (path.stem, cores) in completed:
                row = completed[path.stem, cores]
                assert row['input_sha256'] == sha256(path)
                assert row['plan_sha256'] == sha256(target)
                assert row['repeat_plan_sha256']==sha256(args.out/row['repeat_plan'])
                assert row['repeat_structure_passed']
                if not row['repeat_identical']:
                    assert row['repeat_official_valid'] and row['repeat_result_sha256']==sha256(args.out/row['repeat_result'])
                    report['scoring_performed']=True
                continue
            generation = generate(args.solver, path, cores, target, observe=True)
            first = target.read_bytes()
            check_plan(graph, json.loads(first), cores)
            repeat_target=target.with_name(target.stem+'_repeat.json')
            generate(args.solver, path, cores, repeat_target)
            check_plan(graph,json.loads(repeat_target.read_text(encoding='utf-8')),cores)
            assert generation['sampled_max_process_threads'] <= 10
            row = dict(case=path.stem, cores=cores, input_sha256=sha256(path),
                       plan_sha256=hashlib.sha256(first).hexdigest(), structure_passed=True,
                       repeat_identical=first==repeat_target.read_bytes(),repeat_structure_passed=True,
                       repeat_plan=repeat_target.name,repeat_plan_sha256=sha256(repeat_target),generation=generation)
            if not row['repeat_identical']:
                root=Path(__file__).resolve().parents[1];reference=root/'references/hackathon_20261030'
                manifest=json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
                assert all(sha256(reference/name)==digest for name,digest in manifest['files'].items())
                result_path=repeat_target.with_suffix('.result.json')
                command=[sys.executable,str(reference/'code/multicore_cut_evaluate_problem_2.py'),str(path.resolve()),str(repeat_target.resolve()),
                         '--config',str(reference/'data/config.txt'),'-o',str(result_path.resolve()),
                         '--trace-output',str(repeat_target.with_suffix('.trace.json').resolve()),
                         '--log-output',str(repeat_target.with_suffix('.log.txt').resolve())]
                result=subprocess.run(command,capture_output=True,timeout=600,env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1'))
                repeat_target.with_suffix('.stdout.txt').write_bytes(result.stdout)
                repeat_target.with_suffix('.stderr.txt').write_bytes(result.stderr)
                assert result.returncode==0,result.stderr.decode('utf-8',errors='replace')
                score=json.loads(result_path.read_text(encoding='utf-8'))
                assert score['scene']=='B' and score['num_cores']==cores
                row.update(repeat_official_valid=True,repeat_result=result_path.name,repeat_result_sha256=sha256(result_path),
                           repeat_cycles=score['makespan'],repeat_evaluation_command=command)
                report['scoring_performed']=True
            if args.previous_output_dir:
                previous = args.previous_output_dir / target.name
                assert previous.is_file(), previous
                row['same_as_v0_3_0'] = first == previous.read_bytes()
                row['v0_3_0_plan_sha256'] = sha256(previous)
            report['rows'].append(row)
            report['rows'].sort(key=lambda row: (row['case'], row['cores']))
            (args.out/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(f'{path.stem} n{cores}: both structures passed; repeat identical={row["repeat_identical"]}', flush=True)
    print(f'{len(report["rows"])} output checks passed; scorer invoked for differing repeats={report["scoring_performed"]}.')


if __name__ == '__main__':
    main()
