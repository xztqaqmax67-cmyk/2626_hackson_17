"""Rule-focused generated DAG checks, independent of the public benchmark graphs.

Finite examples are regression evidence, not a proof of legality for every input.
No fixture, expected plan, timing target, or public score is used by the solver.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import time

from check_rules_outputs import check_plan


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def graph_from_dag(cycles, links, *, sizes=None, pipes=None, memory='UB', shared_input=False):
    """Tensor per computation, explicit original boundary COPY nodes."""
    n = len(cycles)
    sizes = sizes or [4096] * n
    pipes = pipes or ['PIPE_M'] * n
    ops = [dict(id=i, op='COMPUTE', pipe=pipes[i], cycles=cycles[i]) for i in range(n)]
    tensors = [dict(id=1000+i, pos=memory, size=sizes[i]) for i in range(n)]
    edges = [dict(source=i, target=1000+i) for i in range(n)]
    for a, b in links:
        edges.append(dict(source=1000+a, target=b))
    sources = [i for i in range(n) if not any(b == i for _, b in links)]
    sinks = [i for i in range(n) if not any(a == i for a, _ in links)]
    tensors += [dict(id=2000, pos='DDR', size=1024), dict(id=2001, pos=memory, size=1024)]
    ops.append(dict(id=3000, op='COPY_IN', pipe='PIPE_MTE2', cycles=-1))
    edges += [dict(source=2000, target=3000), dict(source=3000, target=2001)]
    for i in sources if shared_input else sources[:1]:
        edges.append(dict(source=2001, target=i))
    for i in sinks:
        tensors.append(dict(id=4000+i, pos='DDR', size=sizes[i]))
        ops.append(dict(id=5000+i, op='COPY_OUT', pipe='PIPE_MTE3', cycles=-1))
        edges += [dict(source=1000+i, target=5000+i), dict(source=5000+i, target=4000+i)]
    return dict(ops=ops, tensors=tensors, edges=edges)


def relabel_and_permute(graph, seed):
    graph = copy.deepcopy(graph)
    rng = random.Random(seed)
    ids = [x['id'] for x in graph['ops'] + graph['tensors']]
    mapping = dict(zip(ids, rng.sample(range(10000, 1000000), len(ids))))
    for item in graph['ops'] + graph['tensors']:
        item['id'] = mapping[item['id']]
    for edge in graph['edges']:
        edge['source'], edge['target'] = mapping[edge['source']], mapping[edge['target']]
    for field in ('ops', 'tensors', 'edges'):
        rng.shuffle(graph[field])
    return graph


def fixtures():
    cases = {}
    cases['fork_join'] = graph_from_dag([400, 2400, 800, 2400, 800, 300],
                                       [(0, 1), (0, 2), (1, 3), (2, 4), (3, 5), (4, 5)])
    cases['unequal_branches'] = graph_from_dag([10, 30000, 50, 25000, 20, 100],
                                              [(0, 1), (0, 2), (1, 3), (2, 4), (3, 5), (4, 5)])
    cases['skip_edges'] = graph_from_dag([200, 1000, 1200, 600, 800, 400, 200],
                                        [(0, 1), (0, 2), (1, 3), (2, 4), (3, 5), (4, 5),
                                         (0, 5), (1, 6), (5, 6)])
    cases['shared_external_input'] = graph_from_dag([8000, 6000, 5000, 4000, 100],
                                                   [(0, 2), (1, 3), (2, 4), (3, 4)], shared_input=True)
    writers = graph_from_dag([200, 300, 2000, 100, 300], [(0, 1), (1, 2), (2, 3), (3, 4)])
    writers['tensors'].append(dict(id=6000, pos='UB', size=32768))
    writers['edges'] += [dict(source=0, target=6000), dict(source=3, target=6000),
                         dict(source=6000, target=4)]
    cases['writers_across_phases'] = writers
    cases['zero_cycles_mixed_pipes'] = graph_from_dag(
        [0, 0, 10, 4000, 0, 2], [(0, 2), (1, 3), (2, 4), (3, 4), (4, 5)],
        pipes=['PIPE_M', 'PIPE_V', 'PIPE_FIX', 'PIPE_MTE2', 'PIPE_MTE3', 'PIPE_V'], shared_input=True)
    # Two protected 60000-byte UB tensors exceed the 90% soft threshold,
    # but fit the 131072-byte hard capacity. No required operand is evictable.
    cases['ub_soft_threshold'] = graph_from_dag([600, 600, 600, 600], [(0, 1), (1, 2), (2, 3)],
                                              sizes=[60000] * 4)
    # A partial output may need eviction while an unrelated tensor is live.
    partial = graph_from_dag([50, 50, 50, 50, 50], [(1, 2), (0, 3)],
                            sizes=[1024, 80000, 1024, 1024, 1024])
    partial['tensors'].append(dict(id=6000, pos='UB', size=80000))
    partial['edges'] += [dict(source=0, target=6000), dict(source=3, target=6000),
                         dict(source=6000, target=4), dict(source=2, target=3)]
    cases['partial_writer_pressure'] = partial
    for seed in range(6):
        rng = random.Random(85000 + seed)
        n = 14 + seed * 2
        links = set()
        for v in range(1, n):
            for u in rng.sample(range(v), rng.randint(0, min(3, v))):
                links.add((u, v))
        g = graph_from_dag([rng.randint(0, 18000) for _ in range(n)], sorted(links),
                           sizes=[rng.choice([1024, 4096, 8192]) for _ in range(n)],
                           pipes=[rng.choice(['PIPE_M', 'PIPE_V', 'PIPE_FIX']) for _ in range(n)],
                           shared_input=True)
        if seed % 2 == 0:
            g['tensors'].append(dict(id=6000, pos='UB', size=8192))
            g['edges'] += [dict(source=0, target=6000), dict(source=1, target=6000),
                           dict(source=6000, target=n-1)]
        cases[f'random_{seed:02d}'] = g
    cases['fork_join_relabelled'] = relabel_and_permute(cases['fork_join'], 9321)
    cases['writers_relabelled'] = relabel_and_permute(cases['writers_across_phases'], 5932)
    return cases


def validate_fixture(graph):
    ids = {item['id'] for item in graph['ops'] + graph['tensors']}
    assert len(ids) == len(graph['ops']) + len(graph['tensors'])
    edges = {(e['source'], e['target']) for e in graph['edges']}
    assert len(edges) == len(graph['edges'])
    successors = {v: [] for v in ids}
    degree = {v: 0 for v in ids}
    for a, b in edges:
        assert a in ids and b in ids
        successors[a].append(b)
        degree[b] += 1
    ready = [v for v in ids if degree[v] == 0]
    count = 0
    while ready:
        v = ready.pop()
        count += 1
        for w in successors[v]:
            degree[w] -= 1
            if degree[w] == 0:
                ready.append(w)
    assert count == len(ids), 'synthetic fixture has a cycle'


def run(command, folder, prefix, timeout, env):
    begin = time.perf_counter()
    with (folder / f'{prefix}_stdout.txt').open('w', encoding='utf-8') as stdout, \
            (folder / f'{prefix}_stderr.txt').open('w', encoding='utf-8') as stderr:
        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=timeout, env=env)
    if result.returncode:
        raise RuntimeError(f'{prefix} exited {result.returncode}: ' +
                           (folder / f'{prefix}_stderr.txt').read_text(encoding='utf-8', errors='replace')[-2000:])
    return time.perf_counter() - begin


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--solver', type=Path)
    p.add_argument('--reference', type=Path, default=root/'references/hackathon_20261030')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--cores', type=int, nargs='+', choices=range(2, 6), default=[2, 3, 4, 5])
    p.add_argument('--generate-only', action='store_true')
    args = p.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    inputs = out/'inputs'
    inputs.mkdir(exist_ok=True)
    graphs = fixtures()
    for name, graph in graphs.items():
        validate_fixture(graph)
        path = inputs/f'{name}.json'
        content = json.dumps(graph, indent=2, sort_keys=True) + '\n'
        if path.exists():
            assert path.read_text(encoding='utf-8') == content, ('fixture changed; use new output directory', name)
        else:
            path.write_text(content, encoding='utf-8')
    if args.generate_only:
        print(f'{len(graphs)} deterministic fixtures generated and acyclic; no solver/scorer executed.')
        return
    if args.solver is None:
        p.error('--solver is required unless --generate-only is used')
    solver = args.solver.resolve()
    reference = args.reference.resolve()
    config = reference/'data/config.txt'
    evaluator = reference/'code/multicore_cut_evaluate_problem_2.py'
    sources = {f'code/{f.name}': sha(f) for f in sorted((reference/'code').glob('*.py'))}
    sources['data/config.txt'] = sha(config)
    manifest = json.loads((root/'reports/rules_manifest.json').read_text(encoding='utf-8'))
    assert all(manifest['files'][name] == value for name, value in sources.items()), 'official files changed'
    report = dict(version='0.8.1', public_benchmark_inputs_used=False,
                  purpose='General structural and rule regression checks; finite tests are not a universal proof.',
                  fixture_count=len(graphs), requested_cores=args.cores,
                  search_mode='fixed', repeat_plan_identity_required=True,
                  solver_executable_sha256=sha(solver), official_source_sha256=sources,
                  variant_pairs=[['fork_join', 'fork_join_relabelled'],
                                 ['writers_across_phases', 'writers_relabelled']], rows=[], completed=False)
    target = out/'report.json'
    if target.exists():
        prior = json.loads(target.read_text(encoding='utf-8'))
        assert prior['solver_executable_sha256'] == report['solver_executable_sha256'], 'solver changed; use new output directory'
        assert prior['official_source_sha256'] == sources
        assert prior['requested_cores'] == args.cores
        report['rows'] = prior['rows']
    done = {(r['fixture'], r['cores']): r for r in report['rows']}
    env = dict(os.environ, PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')
    for name, graph in graphs.items():
        source = inputs/f'{name}.json'
        for cores in args.cores:
            if (name, cores) in done:
                assert done[name, cores]['input_sha256'] == sha(source)
                continue
            folder = out/f'{name}_n{cores}'
            folder.mkdir(exist_ok=True)
            plan = folder/'plan.json'
            row = dict(fixture=name, cores=cores, input_sha256=sha(source), status='error')
            try:
                command = [str(solver), str(source), '-n', str(cores), '-o', str(plan)]
                row['generation_seconds'] = run(command, folder, 'generate', 90, env)
                check_plan(graph, json.loads(plan.read_text(encoding='utf-8')), cores)
                row['structure_passed'] = True
                row['plan_sha256'] = sha(plan)
                repeat_plan = folder/'repeat_plan.json'
                repeat_command = [str(solver), str(source), '-n', str(cores), '-o', str(repeat_plan)]
                row['repeat_generation_seconds'] = run(repeat_command, folder, 'repeat', 90, env)
                assert repeat_plan.read_bytes() == plan.read_bytes(), 'fixed search plan is not deterministic'
                row['repeat_plan_identical'] = True
                command = [sys.executable, str(evaluator), str(source), str(plan), '--config', str(config),
                           '-o', str(folder/'result.json'), '--trace-output', str(folder/'trace.json'),
                           '--log-output', str(folder/'log.txt')]
                row['evaluation_seconds'] = run(command, folder, 'evaluate', 90, env)
                result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
                assert result['scene'] == 'B' and result['num_cores'] == cores
                row.update(status='ok', cycles=result['makespan'], data_movement_bytes=result['data_movement_bytes'],
                           memory_peak_by_core=result['memory_peak_by_core'], result_sha256=sha(folder/'result.json'))
            except Exception as error:
                row['error'] = f'{type(error).__name__}: {error}'
            report['rows'].append(row)
            target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            print(json.dumps({k: row[k] for k in ('fixture', 'cores', 'status')} |
                             ({'error': row['error']} if 'error' in row else {})), flush=True)
    report['completed'] = True
    report['passed'] = sum(r['status'] == 'ok' for r in report['rows'])
    report['failed'] = len(report['rows']) - report['passed']
    target.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f"{report['passed']}/{len(report['rows'])} synthetic official evaluations passed.")
    if report['failed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
