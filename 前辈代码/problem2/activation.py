"""Bounded branch reallocations that activate a currently unused core.

The caller retains the inherited incumbent and decides acceptance with the
unchanged official evaluator.  These candidates are explorations, never a
replacement for the incumbent merely because they use more cores.

For a fixed core count and bounded candidate limit, preprocessing consists of
constant-many O(V+E) scans and O(V log V) unit ordering.  Exact route accounting
and plan construction are performed only for a bounded shortlist.  No trial
clones a growing schedule for every operation or possible migration group.
"""
from __future__ import annotations

from collections import Counter

from .core import Plan, _settings, check_deadline, pipe_loads, route_bytes


def _parts(graph, assignment, source, *, width=None, separators=False, deadline=None):
    """Components induced by one donor core, optionally split at graph junctions."""
    allowed = {v for v in graph.ids if assignment[v] == source}
    if separators:
        allowed = {v for v in allowed
                   if sum(assignment[w] == source for w in graph.preds[v]) < 3
                   and sum(assignment[w] == source for w in graph.succs[v]) < 3}
    bands = None
    if width is not None:
        levels = {}
        for v in graph.id_order:
            check_deadline(deadline)
            levels[v] = 1 + max((levels[p] for p in graph.preds[v]), default=-1)
        bands = {v: levels[v] // width for v in allowed}
    owner, parts = {}, []
    for root in graph.id_order:
        check_deadline(deadline)
        if root not in allowed or root in owner:
            continue
        owner[root] = len(parts)
        parts.append([])
        stack = [root]
        while stack:
            v = stack.pop()
            for w in graph.preds[v] | graph.succs[v]:
                if (w in allowed and w not in owner
                        and (bands is None or bands[w] == bands[root])):
                    owner[w] = owner[root]
                    stack.append(w)
    for v in graph.id_order:
        if v in owner:
            parts[owner[v]].append(v)
    return parts, owner


def _unit_rows(graph, parts, owner, deadline):
    """Pipe work and conservative communication incidence in linear scans.

    Boundary incidence is an ordering proxy, not a predicted official objective.
    Final candidates receive exact pre-spill route-byte accounting separately.
    """
    work, boundary = [Counter() for _ in parts], [0] * len(parts)
    for b, nodes in enumerate(parts):
        check_deadline(deadline)
        for v in nodes:
            work[b][graph.ops[v]["pipe"]] += graph.weight[v]
    for tid in graph.tensors:
        check_deadline(deadline)
        producers, consumers = graph.eligible_producers[tid], graph.eligible_consumers[tid]
        units = {owner[v] for v in producers | consumers if v in owner}
        if not units:
            continue
        outside = any(v not in owner for v in producers | consumers)
        if len(units) > 1 or outside:
            amount = graph.tensors[tid]["size"] * (2 if producers else 1)
            for b in units:
                boundary[b] += amount
    for edge in graph.direct_edges:
        a, b = edge["source"], edge["target"]
        if owner.get(a) != owner.get(b):
            amount = 2 * max(0, int(edge.get("data_size", 0)))
            for v in (a, b):
                if v in owner:
                    boundary[owner[v]] += amount
    return work, boundary


def _balanced_subset(parts, work, boundary, donor_load, fraction, bandwidth, deadline):
    """Select whole units with a bounded greedy packing, preserving cohesion."""
    target = {p: max(1., value * fraction) for p, value in donor_load.items() if value > 0}
    if not target:
        return set()
    # Communication is amortized against useful work rather than ignored when
    # many similarly sized branches share inputs or junction tensors.
    order = sorted(range(len(parts)), key=lambda b: (
        -(sum(work[b].values()) / (1. + .1 * boundary[b] / bandwidth)),
        -max((work[b][p] / target[p] for p in target), default=0), b))
    selected, moved = set(), Counter()
    def loss(row):
        return sum((row[p] - target[p]) ** 2 / target[p] for p in target)
    current = loss(moved)
    for b in order:
        check_deadline(deadline)
        trial = moved + work[b]
        value = loss(trial)
        if value < current:
            selected.update(parts[b])
            moved, current = trial, value
    return selected


def _relocate(plan, moved, target):
    """Split only affected blocks into consecutive runs; preserve global order."""
    blocks, cores, order = {}, {}, []
    next_id = plan.next_block_id
    for b in plan.order:
        nodes = plan.blocks[b]
        start, first = 0, True
        for end in range(1, len(nodes) + 1):
            if end < len(nodes) and (nodes[end] in moved) == (nodes[start] in moved):
                continue
            new = b if first else next_id
            if not first:
                next_id += 1
            blocks[new] = tuple(nodes[start:end])
            cores[new] = target if nodes[start] in moved else plan.core_of[b]
            order.append(new)
            start, first = end, False
    return Plan(blocks, cores, order, next_id, plan.ncores)


def activation_plans(graph, plan, settings=None, deadline=None, limit=8):
    """Yield <= ``limit`` legal reallocations from busy cores onto one idle core.

    Complete donor components, branches between junctions, and depth-band
    components supply cohesive units.  Only the two busiest occupied cores are
    inspected.  Four unit families and two packing targets bound the shortlist
    at sixteen plans, independent of graph size.  Remaining idle cores can be
    activated by calling this generator again after an accepted improvement.
    """
    check_deadline(deadline)
    if limit <= 0:
        return
    plan.validate(graph)
    idle = sorted(set(range(plan.ncores)) - set(plan.core_of.values()))
    if not idle or not graph.ids:
        return
    settings = _settings(settings)
    bandwidth = max(1., settings["bandwidth"])
    target = idle[0]
    owner = plan.owner
    assignment = {v: plan.core_of[owner[v]] for v in graph.ids}
    loads = pipe_loads(graph, plan)
    sources = sorted(set(assignment.values()),
                     key=lambda k: (-max(loads[k].values()), -sum(loads[k].values()), k))[:2]
    base_bytes = route_bytes(graph, plan)
    total = Counter()
    for row in loads:
        total.update(row)
    unique, shortlist = set(), []
    families = (("components", None, False), ("branches", None, True),
                ("stage4", 4, False), ("stage8", 8, False))
    for source in sources:
        count = sum(k == source for k in assignment.values())
        if count < 2:
            continue
        for label, width, separators in families:
            check_deadline(deadline)
            parts, unit_owner = _parts(graph, assignment, source, width=width,
                                       separators=separators, deadline=deadline)
            if not parts:
                continue
            work, boundary = _unit_rows(graph, parts, unit_owner, deadline)
            for fraction in (.5, .35):
                moved = _balanced_subset(parts, work, boundary, loads[source], fraction,
                                         bandwidth, deadline)
                key = frozenset(moved)
                if not moved or len(moved) >= count or key in unique:
                    continue
                unique.add(key)
                candidate = _relocate(plan, moved, target)
                delta = route_bytes(graph, candidate) - base_bytes
                moved_load = Counter()
                for v in moved:
                    moved_load[graph.ops[v]["pipe"]] += graph.ops[v]["cycles"]
                new_load = [dict(row) for row in loads]
                for p, amount in moved_load.items():
                    new_load[source][p] -= amount
                    new_load[target][p] += amount
                bound = max((max(row.values()) for row in new_load), default=0)
                energy = sum(amount * amount / max(1, total[p])
                             for row in new_load for p, amount in row.items())
                rank = (bound + .1 * max(0, delta) / bandwidth,
                        energy + .05 * max(0, delta) / bandwidth, delta)
                name = f"activate_{source}_to_{target}_{label}_{int(100 * fraction)}"
                shortlist.append((rank, name, candidate))
    for _, label, candidate in sorted(shortlist, key=lambda item: (item[0], item[1]))[:limit]:
        check_deadline(deadline)
        yield label, candidate
