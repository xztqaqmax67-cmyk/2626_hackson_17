"""Native operation-level placement and FIFO-aware topological ordering.

This is a bounded list-scheduling heuristic, not a replacement evaluator.
Candidate placements use only four-pipe state and the routes incident on the
candidate operation. They never clone the growing history of scheduled nodes.
Only the unchanged official scene-B evaluator decides whether a seed is better.
"""
from __future__ import annotations

import heapq
import math

from .core import Plan, PIPES, _settings, check_deadline


class _Placement:
    """Incremental optimistic four-pipe sketch with small trial transactions."""

    def __init__(self, graph, ncores, settings):
        self.graph, self.ncores, self.settings = graph, ncores, settings
        self.available = [{p: 0.0 for p in PIPES} for _ in range(ncores)]
        self.fixed = [{p: 0 for p in PIPES} for _ in range(ncores)]
        self.finish, self.core, self.route_end = {}, {}, {}
        self.bytes = 0

    def trial(self, v, core):
        """Return a proposal; all dictionaries here contain only local changes."""
        g, cfg = self.graph, self.settings
        rows, routes = {}, {}
        added_bytes = 0

        def row(k):
            if k not in rows:
                rows[k] = dict(self.available[k])
            return rows[k]

        def copy(key, k, pipe, size, release):
            nonlocal added_bytes
            if key in self.route_end:
                return self.route_end[key]
            if key in routes:
                return routes[key]
            state = row(k)
            end = max(state[pipe], release) + max(1, math.ceil(size / cfg["bandwidth"]))
            state[pipe] = routes[key] = end
            added_bytes += size
            return end

        def transfer(key, source, size, release):
            out = copy(("out", key, source, core), source, "PIPE_MTE3", size, release)
            return copy(("in", key, source, core), core, "PIPE_MTE2", size,
                        out + cfg["cross_core_copy_delay"])

        ready = max((self.finish[p] for p in g.preds[v]), default=0.0)
        for tid in g.op_inputs[v]:
            size = g.tensors[tid]["size"]
            producers = g.eligible_producers[tid]
            if not producers:
                ready = max(ready, copy(("input", tid, core), core, "PIPE_MTE2", size, 0))
                continue
            releases = {}
            for p in producers:
                source = self.core[p]
                releases[source] = max(releases.get(source, 0), self.finish[p])
            for source, release in releases.items():
                ready = max(ready, release if source == core else
                            transfer(("tensor", tid), source, size, release))
        for index, pred, size in g.direct_inputs[v]:
            if self.core[pred] != core:
                ready = max(ready, transfer(("direct", index), self.core[pred],
                                            size, self.finish[pred]))
        pipe = g.ops[v]["pipe"]
        start = max(ready, row(core)[pipe])
        end = start + g.weight[v]
        row(core)[pipe] = end
        # Output COPYs are attached to the last local producer, as in scene B.
        for tid in g.op_outputs[v]:
            if tid not in g.original_output_relations:
                continue
            producers = g.eligible_producers[tid]
            if any(p != v and p not in self.finish for p in producers):
                continue
            releases = {}
            for p in producers:
                source = core if p == v else self.core[p]
                release = end if p == v else self.finish[p]
                releases[source] = max(releases.get(source, 0), release)
            for source, release in releases.items():
                copy(("output", tid, source), source, "PIPE_MTE3",
                     g.tensors[tid]["size"], release)
        return {"op": v, "core": core, "start": start, "end": end,
                "rows": rows, "routes": routes, "added_bytes": added_bytes}

    def commit(self, proposal):
        v, core = proposal["op"], proposal["core"]
        for k, row in proposal["rows"].items():
            self.available[k] = row
        self.route_end.update(proposal["routes"])
        self.finish[v], self.core[v] = proposal["end"], core
        self.fixed[core][self.graph.ops[v]["pipe"]] += self.graph.ops[v]["cycles"]
        self.bytes += proposal["added_bytes"]


def _operation_plan(graph, ncores, settings, *, balance=0.0, window=1,
                    mode="critical", assignment=None, deadline=None,
                    observed_starts=None):
    """O(window * ncores * (V + E) + V log V), fixed small window.

    The priority heap bounds lookahead to ready operations on important paths.
    Within that window an operation whose operands can arrive earlier is issued
    first. This avoids placing a waiting operation in front of executable work
    on the same FIFO. Soft load prices give independent branches a chance to
    occupy other cores without cutting every cheap dependency indiscriminately.
    """
    if ncores < 1:
        raise ValueError("ncores must be positive")
    sketch = _Placement(graph, ncores, settings)
    depth = {}
    for v in graph.id_order:
        depth[v] = max((depth[p] + graph.weight[p] for p in graph.preds[v]), default=0)
    if observed_starts:
        priority = {v: (observed_starts.get(v, float("inf")), -graph.rank[v], v)
                    for v in graph.ids}
    elif mode == "breadth":
        priority = {v: (depth[v], -graph.rank[v], v) for v in graph.ids}
    elif mode == "id":
        priority = {v: (v,) for v in graph.ids}
    else:
        priority = {v: (-graph.rank[v], v) for v in graph.ids}
    degree = {v: len(graph.preds[v]) for v in graph.ids}
    heap = [(priority[v], v) for v in graph.ids if not degree[v]]
    heapq.heapify(heap)
    order = []
    while heap:
        check_deadline(deadline)
        candidates = [heapq.heappop(heap) for _ in range(min(window, len(heap)))]
        proposals = []
        for _, v in candidates:
            pipe = graph.ops[v]["pipe"]
            cores = (assignment[v],) if assignment is not None else range(ncores)
            best = None
            for core in cores:
                trial = sketch.trial(v, core)
                load = max(sketch.fixed[core][p] + (graph.weight[v] if p == pipe else 0)
                           for p in PIPES)
                score = (trial["end"] + balance * load, trial["added_bytes"], load, core)
                if best is None or score < best[0]:
                    best = (score, trial)
            trial = best[1]
            # Earliest-start comparison avoids giving short nodes an advantage.
            proposals.append(((trial["start"], priority[v]), trial))
        _, chosen = min(proposals, key=lambda item: item[0])
        v = chosen["op"]
        for entry in candidates:
            if entry[1] != v:
                heapq.heappush(heap, entry)
        sketch.commit(chosen)
        order.append(v)
        for successor in graph.succs[v]:
            degree[successor] -= 1
            if not degree[successor]:
                heapq.heappush(heap, (priority[successor], successor))
    plan = Plan({v: (v,) for v in graph.ids}, sketch.core, order,
                max(graph.ids, default=-1) + 1, ncores)
    plan.validate(graph)
    return plan


def seed_plans(graph, ncores, settings=None, deadline=None):
    """A small deterministic portfolio with distinct allocation/order biases."""
    settings = _settings(settings)
    seen = set()
    for label, mode, balance, window in (
        ("pipe_critical_eft", "critical", 0.0, 1),
        ("pipe_ready_window", "critical", 0.0, 8),
        ("pipe_balanced_window", "critical", 0.5, 8),
        ("pipe_breadth_balanced", "breadth", 0.5, 8),
    ):
        check_deadline(deadline)
        plan = _operation_plan(graph, ncores, settings, mode=mode,
                               balance=balance, window=window, deadline=deadline)
        key = plan.fingerprint()
        if key not in seen:
            seen.add(key)
            yield label, plan


def reorder_plans(graph, plan, settings=None, result=None, limit=4):
    """Keep operation ownership; expose an improved order as fine subgraphs.

    Dependency legality is checked for every return value. ``result`` is an
    optional official evaluation and provides measured issue-time priorities;
    the remaining variants use bounded readiness lookahead on the fixed map.
    """
    settings = _settings(settings)
    owner = plan.owner
    assignment = {v: plan.core_of[owner[v]] for v in graph.ids}
    seen = {plan.fingerprint()}
    count = 0
    measured = {}
    if result:
        official = result.get("official_result", result)
        for core in official.get("per_core_timeline", []):
            for op in core.get("ops", []):
                if op["op_id"] in graph.eligible:
                    measured[op["op_id"]] = op["start"]
    variants = [("critical", 8), ("breadth", 8), ("id", 4), ("critical", 1)]
    if measured:
        variants.insert(0, ("measured", 8))
    for mode, window in variants:
        if count >= max(0, limit):
            break
        candidate = _operation_plan(graph, plan.ncores, settings, mode=mode,
                                    window=window, assignment=assignment,
                                    observed_starts=measured if mode == "measured" else None)
        key = candidate.fingerprint()
        if key not in seen:
            seen.add(key)
            count += 1
            yield "fifo_%s_%s" % (mode, window), candidate
