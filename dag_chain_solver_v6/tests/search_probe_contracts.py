"""Check the development probe's policy and saved-singleton-plan contracts."""
import argparse
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe', type=Path, required=True)
p.add_argument('--out', type=Path, required=True)
args = p.parse_args()
exe = args.probe.resolve()
root = args.out.resolve()
root.mkdir(parents=True, exist_ok=True)
graph = root / 'graph.json'
graph.write_text(json.dumps(dict(
    ops=[dict(id=11, op='ADD', pipe='PIPE_FIX', cycles=4),
         dict(id=29, op='ADD', pipe='PIPE_FIX', cycles=7)],
    tensors=[dict(id=1001, pos='UB', size=16)],
    edges=[dict(source=11, target=1001), dict(source=1001, target=29)])),
    encoding='utf-8')
seed = root / 'seed.json'
valid = dict(node_to_subgraph={'11': 11, '29': 29}, core_schedules=[[11, 29], []])


def write_seed(value):
    seed.write_text(json.dumps(value), encoding='utf-8')


def run(policy='1', mode='resume', *, ok=True, error=None):
    # Zero search budget exercises seed import and output without a timing test.
    command = [str(exe), str(graph), '2', '0', policy, mode, 'plan.json']
    if mode == 'resume':
        command.append(str(seed))
    result = subprocess.run(command, cwd=root, capture_output=True, text=True,
                            encoding='utf-8', timeout=30)
    assert (result.returncode == 0) == ok, (command, result.stdout, result.stderr)
    if error:
        assert error in result.stderr, result.stderr
    if ok:
        stats = json.loads(result.stdout)
        assert stats['budget_seconds'] == 0
        assert stats['mode'] == mode
        assert stats['search_seconds'] >= 0
        assert not {'fastest_cycles', 'fastest_bytes', 'traffic_tradeoff'} & stats.keys()
        plan = json.loads((root / 'plan.json').read_text(encoding='utf-8'))
        assert plan['node_to_subgraph'] == valid['node_to_subgraph']
        assert sorted(v for row in plan['core_schedules'] for v in row) == [11, 29]
        if mode == 'resume':
            assert plan == valid


write_seed(valid)
for policy in ['0', '1']:
    run(policy)
run(mode='full')
for policy in ['2', '4', '7', '-1', '1junk']:
    run(policy, ok=False, error='policy must be 0 or 1')

for mapping in [
    {'11': 11, '29': 11},          # Grouped subgraphs.
    {'11': 29, '29': 11},          # Singleton subgraphs with relabelled IDs.
    {'11': 11},                    # Missing operator.
    {'11': 11, '29': 29, '99': 99},
    {'11': 11, '99': 99},          # Unknown operator with the right entry count.
    {'011': 11, '29': 29},         # Noncanonical ID spelling.
    [],
]:
    write_seed(dict(valid, node_to_subgraph=mapping))
    run(ok=False, error='singleton identity mapping')
write_seed(dict(core_schedules=valid['core_schedules']))
run(ok=False, error='missing JSON field: node_to_subgraph')
for streams in [[[11], []], [[11, 29], [29]], [[29, 11], []], [[11, 29]]]:
    write_seed(dict(valid, core_schedules=streams))
    run(ok=False)
print('Search probe contracts passed: policies 0/1, singleton seed identity, schedules and zero-budget output')
