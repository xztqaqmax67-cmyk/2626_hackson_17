#!/usr/bin/env python3
"""Standalone CLI used by score.py; no third-party Python dependencies."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from collections import defaultdict

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from local_eval import VERSION
from local_eval.model import Graph, build, read_config
from local_eval.memory import insert_spills
from local_eval.simulator import audit, simulate


def evaluate(raw, plan, capacity, bandwidth, delay, sync_mode='producer_consumer', trace_output=False):
    graph = Graph(raw)
    if plan is None:
        # One fixed subgraph: Step 2 reverse DFS selects the single-core order.
        plan = {'node_to_subgraph': {str(n): 0 for n in sorted(graph.compute)},
                'core_schedules': [[0] if graph.compute else []]}
    program, sequences = build(graph, plan, delay, sync_mode)
    epochs, expanded = insert_spills(program, sequences, capacity)
    priorities = {n: i for seq in expanded for i, n in enumerate(seq)}
    fixed_orders = {}
    for seq in expanded:
        local_nodes = {n: program.nodes[n] for n in seq}
        # Establish per-pipe order from the legal priority sequence. Across
        # different pipes, operations still issue independently when ready.
        # Treating remote COPY_INs as ready during speculative local simulation
        # can otherwise move a later MTE3 compute before an earlier COPY_OUT and
        # introduce a cross-core wait cycle when the pipe order is frozen.
        orders = defaultdict(list)
        for n in seq:
            node = program.nodes[n]
            orders[node.core, node.pipe].append(n)
        fixed_orders.update(orders)
        for order in orders.values():
            for a, b in zip(order, order[1:]):
                program.nodes[b].depend(a)
    for a, b, lag in program.cross:
        program.nodes[b].depend(a, lag)
    trace, _, moved = simulate(program.nodes, bandwidth, priorities)
    checks = audit(program, epochs, trace, fixed_orders, capacity, bandwidth, moved)
    total = sum(n.size for n in program.nodes.values() if n.ddr)
    spills = sum(n.size for n in program.nodes.values() if n.category in ('spill_in', 'spill_out'))
    result = {
        'evaluator_version': VERSION, 'is_official': False,
        'makespan': max((v['end'] for v in trace.values()), default=0),
        'total_ddr_bytes': total,
        'data_movement_bytes': {'scheduled_copy_bytes': total, 'spill_added_copy_bytes': spills,
                                'pre_spill_copy_bytes': total - spills},
        'op_count': len(program.nodes), 'spill_op_count': sum(n.category.startswith('spill_')
                                                            for n in program.nodes.values()),
        'model': {'capacity': capacity, 'bandwidth_bytes_per_cycle': bandwidth,
                  'sync_delay_cycles': delay, 'sync_mode': sync_mode,
                  'single_core_order': 'one-subgraph fixed reverse DFS',
                  'assumptions': ['fractional cycles; deterministic tie breaks',
                                  'one source writeback shared by all destination cores and final output',
                                  'all writers of a tensor must be on one core; partial writes preserve contents',
                                  'virtual-byte memory reuse follows serial priority order; may constrain overlap',
                                  'per-pipe priority order is fixed before cross-core simulation; pipes overlap independently',
                                  'direct DDR compute operands use UB, inherited from old Scene B',
                                  'boundary COPY nodes are regenerated for each placement']},
        **checks,
    }
    if trace_output:
        result['timeline'] = [dict(id=n, origin=node.origin, core=node.core, op=node.name,
                                   pipe=node.pipe, category=node.category, bytes=node.size if node.ddr else 0,
                                   start=trace[n]['start'], end=trace[n]['end'])
                              for n, node in program.nodes.items()]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--graph', type=Path, required=True)
    parser.add_argument('--plan', type=Path)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--mode', choices=['single', 'multi'], default='multi')
    parser.add_argument('--sync-mode', choices=['producer_consumer', 'copy_chain'], default='producer_consumer')
    parser.add_argument('--trace', action='store_true')
    args = parser.parse_args()
    if args.mode == 'multi' and args.plan is None:
        parser.error('--plan is required in multi mode')
    capacity, bandwidth, delay = read_config(args.config)
    raw_bytes = args.graph.read_bytes()
    raw = json.loads(raw_bytes)
    plan = json.loads(args.plan.read_text(encoding='utf-8-sig')) if args.mode == 'multi' else None
    result = evaluate(raw, plan, capacity, bandwidth, delay, args.sync_mode, args.trace)
    result['input_graph'] = args.graph.name
    result['input_sha256'] = hashlib.sha256(raw_bytes).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f"makespan={result['makespan']:.6f} DDR={result['total_ddr_bytes']} audit=passed")


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError) as exc:
        print(f'[evaluation error] {exc}', file=sys.stderr)
        sys.exit(1)
