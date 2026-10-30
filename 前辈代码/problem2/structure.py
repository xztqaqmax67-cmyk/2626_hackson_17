"""Linear graph structure extraction and bounded, diverse scene-B seeds.

Macroscopic allocation units are independent branches within monotone depth
bands.  The units remain intact while assigning cores; the output can expose
each branch or merge independent units on the same core within a band.  This
separates allocation from the order constraints seen by the official evaluator.
No result files or problem-1 implementation are used.
"""
from __future__ import annotations

from collections import Counter, defaultdict

from .core import Plan, _topological_blocks, block_dependencies, check_deadline


def _levels(graph):
    level = {}
    for v in graph.id_order:
        level[v] = 1 + max((level[p] for p in graph.preds[v]), default=-1)
    return level


def stage_branches(graph, width=None, *, reverse=False, deadline=None):
    """Return topological branches and their monotone stages in O(V+E).

    ``width=None`` returns full weak components.  Reverse bands align cuts to
    reductions, whereas forward bands align them to graph inputs.
    """
    if width is not None and width < 1:
        raise ValueError("stage width must be positive")
    if width is None:
        return list(graph.atoms), [0] * len(graph.atoms)
    if reverse:
        height = {}
        for v in reversed(graph.id_order):
            height[v] = 1 + max((height[s] for s in graph.succs[v]), default=-1)
        stage = {v: -(height[v] // width) for v in graph.ids}
    else:
        stage = {v: depth // width for v, depth in _levels(graph).items()}
    owner, parts, stages = {}, [], []
    for root in graph.id_order:
        check_deadline(deadline)
        if root in owner:
            continue
        b = len(parts)
        parts.append([])
        stages.append(stage[root])
        owner[root] = b
        stack = [root]
        while stack:
            v = stack.pop()
            for w in graph.preds[v] | graph.succs[v]:
                if w not in owner and stage[w] == stage[root]:
                    owner[w] = b
                    stack.append(w)
    for v in graph.id_order:
        parts[owner[v]].append(v)
    return [tuple(part) for part in parts], stages


def reverse_dfs_order(graph):
    """Iterative predecessor DFS; complete consumer closures before another sink."""
    seen, result = set(), []
    roots = sorted(graph.ids, key=lambda v: (bool(graph.succs[v]), -graph.position[v], v))
    for root in roots:
        if root in seen:
            continue
        stack = [(root, False)]
        while stack:
            v, expanded = stack.pop()
            if expanded:
                if v not in seen:
                    seen.add(v)
                    result.append(v)
                continue
            if v in seen:
                continue
            stack.append((v, True))
            # Deep/high-work predecessor closures are emitted first.
            stack.extend((p, False) for p in sorted(graph.preds[v],
                         key=lambda p: (graph.rank[p], -p)))
    return result


def _part_data(graph, parts):
    owner = {v: b for b, nodes in enumerate(parts) for v in nodes}
    work, incoming, external = [], [], []
    for b, nodes in enumerate(parts):
        work.append(Counter())
        inc, ext = Counter(), set()
        for v in nodes:
            work[b][graph.ops[v]["pipe"]] += graph.weight[v]
            for tid in graph.op_inputs[v]:
                producers = graph.eligible_producers[tid]
                if not producers:
                    ext.add(tid)
                else:
                    for source in {owner[p] for p in producers} - {b}:
                        inc[source, tid] = graph.tensors[tid]["size"]
            for _, pred, size in graph.direct_inputs[v]:
                if owner[pred] != b:
                    inc[owner[pred], -v - 1] += size
        incoming.append(inc)
        external.append(ext)
    return work, incoming, external


def _pack(graph, parts, stages, ncores, settings, coefficient, deadline):
    """LPT with Pipe-specific convex load cost and exact input reuse increments.

    For stage branches, every incoming other-part dependency is in an earlier
    stage, so assigning all earlier stages first also makes affinity available.
    The computation costs O((V+E)K + A log A), K being the number of cores.
    """
    work, incoming, external = _part_data(graph, parts)
    totals = Counter()
    for row in work:
        totals.update(row)
    target = {p: max(1., amount / ncores) for p, amount in totals.items()}
    load = [Counter() for _ in range(ncores)]
    input_seen = [set() for _ in range(ncores)]
    assignment, by_stage = {}, defaultdict(list)
    for b, stage in enumerate(stages):
        by_stage[stage].append(b)
    for stage in sorted(by_stage):
        # The earliest branches should establish distinct processing lanes.
        units = sorted(by_stage[stage], key=lambda b:
                       (-max((w / target[p] for p, w in work[b].items()), default=0),
                        -sum(work[b].values()), b))
        for b in units:
            check_deadline(deadline)
            def cost(core):
                balance = sum((2 * load[core][p] * w + w * w) / target[p]
                              for p, w in work[b].items())
                extra = sum(graph.tensors[t]["size"] for t in external[b]
                            if t not in input_seen[core])
                extra += sum(2 * size for (source, _), size in incoming[b].items()
                             if assignment[source] != core)
                return (balance + coefficient * extra / settings["bandwidth"],
                        sum(load[core].values()), core)
            core = min(range(ncores), key=cost)
            assignment[b] = core
            load[core].update(work[b])
            input_seen[core].update(external[b])
    return assignment


def _emit(graph, parts, stages, assignment, ncores, merged, deadline=None):
    """Coalesce only independent units within a stage; quotient stays acyclic."""
    check_deadline(deadline)
    groups, cores = {}, {}
    if merged:
        buckets = defaultdict(list)
        for b, nodes in enumerate(parts):
            buckets[stages[b], assignment[b]].extend(nodes)
        for stage_core, nodes in sorted(buckets.items()):
            b = len(groups)
            groups[b] = tuple(sorted(nodes, key=graph.position.__getitem__))
            cores[b] = stage_core[1]
    else:
        groups = {b: nodes for b, nodes in enumerate(parts)}
        cores = dict(assignment)
    preds, succs = block_dependencies(graph, groups)
    priority = {b: (-max((graph.rank[v] for v in nodes), default=0), b)
                for b, nodes in groups.items()}
    order = _topological_blocks(preds, succs, priority)
    plan = Plan(groups, cores, order, len(groups), ncores)
    plan.validate(graph)
    return plan


def _dfs_seed(graph, ncores, settings, count, deadline):
    """Balanced DFS intervals with predecessor affinity, no quadratic sketch copies."""
    order = reverse_dfs_order(graph)
    total = sum(graph.weight.values())
    target = max(1, total / count)
    parts, part, weight = [], [], 0
    for v in order:
        if part and weight + graph.weight[v] > target:
            parts.append(tuple(part))
            part, weight = [], 0
        part.append(v)
        weight += graph.weight[v]
    if part:
        parts.append(tuple(part))
    # Each interval is its own stage; incoming dependencies are already placed.
    stages = list(range(len(parts)))
    assignment = _pack(graph, parts, stages, ncores, settings, .1, deadline)
    return _emit(graph, parts, stages, assignment, ncores, False, deadline)


def _separator_assignment(graph, ncores, settings, threshold, deadline):
    """Expose long branches by separating high fan-out synchronization nodes.

    Removed vertices are still scheduled exactly once.  They are placed after
    branch packing using affinity to both producers and consumers.  This is an
    allocation heuristic only: regions are never contracted as schedule blocks,
    because removal of a separator does not guarantee an acyclic quotient.
    """
    separators = {v for v in graph.ids if len(graph.succs[v]) >= threshold}
    if not separators:
        return None
    seen = set(separators)
    groups = []
    for root in graph.id_order:
        check_deadline(deadline)
        if root in seen:
            continue
        stack, nodes = [root], []
        seen.add(root)
        while stack:
            v = stack.pop()
            nodes.append(v)
            for w in graph.preds[v] | graph.succs[v]:
                if w not in seen:
                    seen.add(w)
                    stack.append(w)
        groups.append(nodes)
    if len(groups) < ncores:
        return None
    work = [Counter() for _ in groups]
    total = Counter()
    for v in graph.ids:
        total[graph.ops[v]["pipe"]] += graph.weight[v]
    target = {p: max(1., w / ncores) for p, w in total.items()}
    for b, nodes in enumerate(groups):
        for v in nodes:
            work[b][graph.ops[v]["pipe"]] += graph.weight[v]
    loads, assignment = [Counter() for _ in range(ncores)], {}
    for b in sorted(range(len(groups)), key=lambda b: (-sum(work[b].values()), b)):
        core = min(range(ncores), key=lambda k:
                   (sum((2 * loads[k][p] * w + w * w) / target[p]
                        for p, w in work[b].items()), k))
        for v in groups[b]:
            assignment[v] = core
        loads[core].update(work[b])
    for v in graph.id_order:
        if v not in separators:
            continue
        check_deadline(deadline)
        affinity = Counter()
        for w in graph.preds[v] | graph.succs[v]:
            if w in assignment:
                affinity[assignment[w]] += (
                    graph.edge_bytes.get((v, w), 0) + graph.edge_bytes.get((w, v), 0)
                ) / settings["bandwidth"] + settings["cross_core_copy_delay"]
        pipe, weight = graph.ops[v]["pipe"], graph.weight[v]
        core = min(range(ncores), key=lambda k:
                   ((2 * loads[k][pipe] * weight + weight * weight) / target[pipe]
                    - .5 * affinity[k], k))
        assignment[v] = core
        loads[core][pipe] += weight
    return assignment


def _allocation_plan(graph, assignment, ncores, mode):
    """Safe ordering refinement independent of a possibly cyclic region graph."""
    order = reverse_dfs_order(graph) if mode == "dfs" else graph.topological_order(mode)
    blocks, core_of, current, previous = {}, {}, [], None
    for v in order:
        core = assignment[v]
        if current and core != previous:
            b = len(blocks)
            blocks[b], core_of[b] = tuple(current), previous
            current = []
        current.append(v)
        previous = core
    if current:
        b = len(blocks)
        blocks[b], core_of[b] = tuple(current), previous
    plan = Plan(blocks, core_of, list(blocks), len(blocks), ncores)
    plan.validate(graph)
    return plan


def seed_plans(graph, ncores, settings, deadline=None):
    """Yield a small deterministic portfolio, with the cheapest branch seeds first.

    Every graph scan is linear except sorting at most V items; candidate core
    costs are O(K(V+E)).  At most 12 distinct plans are returned.  The generator
    checks the shared deadline between units and does no official evaluations.
    """
    if ncores < 1:
        raise ValueError("ncores must be positive")
    check_deadline(deadline)
    if not graph.ids:
        yield "structure_empty", Plan({}, {}, [], 0, ncores)
        return
    seen = set()

    def fresh(plan):
        signature = plan.fingerprint()
        if signature in seen:
            return False
        seen.add(signature)
        return True

    atom_work = [sum(graph.weight[v] for v in part) for part in graph.atoms]
    dominant = max(atom_work) > sum(atom_work) / max(2, ncores)
    if len(graph.atoms) >= ncores and not dominant:
        parts, stages = stage_branches(graph)
        for coefficient in (0., .5):
            assignment = _pack(graph, parts, stages, ncores, settings, coefficient, deadline)
            for merged in (True, False):
                plan = _emit(graph, parts, stages, assignment, ncores, merged, deadline)
                if fresh(plan):
                    yield f"structure_atoms_c{coefficient:g}_{'merged' if merged else 'branches'}", plan
    if dominant:
        depth = max(_levels(graph).values(), default=0) + 1
        for threshold in (4, 8):
            assignment = _separator_assignment(graph, ncores, settings, threshold, deadline)
            if assignment is not None:
                for mode in (("id", "locality") if threshold == 4 else ("id",)):
                    plan = _allocation_plan(graph, assignment, ncores, mode)
                    if fresh(plan):
                        yield f"structure_separator{threshold}_{mode}", plan
        # Width 8 retains the long parallel chains preceding binary reductions.
        # Finer bands separate fork/join stages in large, connected attention DAGs.
        for width, reverse, coefficient in ((4, False, .1), (8, False, .5),
                                             (16, False, .5), (8, True, .5)):
            if width >= depth and len(graph.atoms) >= ncores:
                continue
            parts, stages = stage_branches(graph, width, reverse=reverse, deadline=deadline)
            assignment = _pack(graph, parts, stages, ncores, settings, coefficient, deadline)
            for merged in ((True, False) if width in (4, 8) and not reverse else (True,)):
                plan = _emit(graph, parts, stages, assignment, ncores, merged, deadline)
                if fresh(plan):
                    yield f"structure_stage{width}_{'reverse' if reverse else 'forward'}_{'merged' if merged else 'branches'}", plan
        if len(graph.atoms) >= ncores:
            parts, stages = stage_branches(graph)
            assignment = _pack(graph, parts, stages, ncores, settings, 0., deadline)
            plan = _emit(graph, parts, stages, assignment, ncores, True, deadline)
            if fresh(plan):
                yield "structure_atoms_fallback", plan
        for multiple in (4,):
            check_deadline(deadline)
            plan = _dfs_seed(graph, ncores, settings, ncores * multiple, deadline)
            if fresh(plan):
                yield f"structure_dfs_{multiple}k", plan
