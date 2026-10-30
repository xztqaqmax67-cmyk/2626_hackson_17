"""Cheap first native assignment for scene B's merged per-core tasks."""
from .core import Plan


def first_plan(graph, ncores):
    # Minimize the size of the first merged tasks. Component placement creates
    # no extra compute dependencies. If one component dominates, contiguous
    # topological intervals make all cross-core dependencies point forward.
    count = len(graph.id_order)
    if not count:
        return Plan({}, {}, [], 0, ncores)
    blocks = {k: [] for k in range(min(ncores, count))}
    if len(graph.atoms) >= ncores and max(map(len, graph.atoms)) <= count / ncores * 1.5:
        loads = [0] * ncores
        owners = {}
        for atom in sorted(graph.atoms, key=lambda row: (-len(row), row[0])):
            target = min(range(ncores), key=lambda k: (loads[k], k))
            loads[target] += len(atom)
            owners.update((op, target) for op in atom)
        for op in graph.id_order:
            blocks[owners[op]].append(op)
    else:
        for index, op in enumerate(graph.id_order):
            blocks[index * len(blocks) // count].append(op)
    blocks = {k: tuple(ops) for k, ops in blocks.items() if ops}
    plan = Plan(blocks, {k: k for k in blocks}, list(blocks), max(blocks) + 1, ncores)
    plan.validate(graph)
    return plan
