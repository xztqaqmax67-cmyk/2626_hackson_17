"""Native scene-B construction and legal block operations (Steps 1--3).

The four-pipe sketch is an ordering heuristic, never an execution simulator.
It omits spills and shared-DDR contention; every accepted plan must pass the
unchanged official scene-B evaluator. Original graph objects are read only.
"""
from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
import hashlib
import heapq
import json
import math
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
OFFICIAL_DIR = Path(__file__).resolve().parent / "official_code"
if str(OFFICIAL_DIR) not in sys.path:
    sys.path.insert(0, str(OFFICIAL_DIR))
from evaluation_validation import validate_graph, read_evaluation_config, read_required_settings
from stub_multicore_cut_and_schedule import _build_op_adjacency, _contract_excluded_copy_nodes
from schedule_step3 import PIPES


def check_deadline(deadline):
    if deadline is not None and time.monotonic() >= deadline:
        raise TimeoutError("problem2 construction reached its deadline")


def _settings(settings=None):
    if settings is not None:
        return settings
    raise ValueError("explicit parsed hardware settings are required")


class GraphInfo:
    """Tensor identities and true dependencies; sharing alone creates no edge."""

    def __init__(self, graph):
        validate_graph(graph)
        self.graph = self.original_graph = graph
        self.ops = {o["id"]: o for o in graph["ops"]}
        self.tensors = {t["id"]: t for t in graph["tensors"]}
        self.ids = sorted(i for i, o in self.ops.items() if o["op"] not in {"COPY_IN", "COPY_OUT"})
        self.eligible = set(self.ids)
        self.compute_ops = self.ids
        _, succs = _build_op_adjacency(graph)
        self.preds, self.succs = _contract_excluded_copy_nodes(self.ids, succs)
        self.predecessors, self.successors = self.preds, self.succs
        self.producers, self.consumers = defaultdict(set), defaultdict(set)
        ins, outs = defaultdict(list), defaultdict(list)
        self.direct_edges = []
        for edge in graph["edges"]:
            a, b = edge["source"], edge["target"]
            if a in self.ops and b in self.tensors:
                self.producers[b].add(a)
                outs[a].append(b)
            elif a in self.tensors and b in self.ops:
                self.consumers[a].add(b)
                ins[b].append(a)
            elif a in self.ops and b in self.ops:
                self.direct_edges.append(edge)
        self.op_inputs = {v: tuple(dict.fromkeys(ins[v])) for v in self.ids}
        self.op_outputs = {v: tuple(dict.fromkeys(outs[v])) for v in self.ids}
        self.eligible_producers = {t: self.producers[t] & self.eligible for t in self.tensors}
        self.eligible_consumers = {t: self.consumers[t] & self.eligible for t in self.tensors}
        self.external_input_groups = {
            t: set(self.eligible_consumers[t]) for t in self.tensors
            if self.eligible_consumers[t] and not self.eligible_producers[t]
        }
        self.original_output_relations = {
            t: set(self.eligible_producers[t]) for t in self.tensors
            if self.eligible_producers[t] and (
                not self.eligible_consumers[t] or
                any(self.ops[v]["op"] == "COPY_OUT" for v in self.consumers[t])
            )
        }
        self.tensor_info = self.tensors
        self.weight = {v: max(1, self.ops[v]["cycles"]) for v in self.ids}
        # Raw zero cycles stay zero in true fixed-work lower bounds.
        self.fixed_work_by_pipe = {v: {self.ops[v]["pipe"]: self.ops[v]["cycles"]} for v in self.ids}
        self.edge_bytes = defaultdict(int)
        for tid, producers in self.eligible_producers.items():
            for a in producers:
                for b in self.eligible_consumers[tid]:
                    self.edge_bytes[a, b] += self.tensors[tid]["size"]
        self.direct_inputs = defaultdict(list)
        for index, edge in enumerate(self.direct_edges):
            a, b = edge["source"], edge["target"]
            if a in self.eligible and b in self.eligible:
                self.direct_inputs[b].append((index, a, max(0, int(edge.get("data_size", 0)))))
                self.edge_bytes[a, b] += max(0, int(edge.get("data_size", 0)))
        self.rank = {}
        self.id_order = self._kahn("id")
        self.position = {v: i for i, v in enumerate(self.id_order)}
        for v in reversed(self.id_order):
            self.rank[v] = self.weight[v] + max((self.rank[s] for s in self.succs[v]), default=0)
        self.rank_up = self.rank
        self.atoms, self.atom_of = [], {}
        for root in self.id_order:
            if root in self.atom_of:
                continue
            index, stack = len(self.atoms), [root]
            self.atoms.append([])
            self.atom_of[root] = index
            while stack:
                v = stack.pop()
                for other in self.preds[v] | self.succs[v]:
                    if other not in self.atom_of:
                        self.atom_of[other] = index
                        stack.append(other)
        for v in self.id_order:
            self.atoms[self.atom_of[v]].append(v)
        self.atoms = [tuple(a) for a in self.atoms]

    @classmethod
    def from_path(cls, path):
        with open(path, encoding="utf-8") as handle:
            return cls(json.load(handle))

    def _kahn(self, mode):
        degree = {v: len(self.preds[v]) for v in self.ids}
        priority = lambda v: (0 if mode == "id" else -self.rank[v], v)
        heap = [priority(v) for v in self.ids if not degree[v]]
        heapq.heapify(heap)
        ready = {v for v in self.ids if not degree[v]}
        order, last = [], None
        while ready:
            nearby = self.succs[last] & ready if last is not None and mode == "locality" else set()
            if nearby:
                v = min(nearby, key=lambda w: (-self.edge_bytes[last, w], -self.rank[w], w))
            else:
                while heap and heap[0][1] not in ready:
                    heapq.heappop(heap)
                v = heapq.heappop(heap)[1]
            ready.remove(v)
            order.append(v)
            last = v
            for w in sorted(self.succs[v]):
                degree[w] -= 1
                if not degree[w]:
                    ready.add(w)
                    heapq.heappush(heap, priority(w))
        if len(order) != len(self.ids):
            raise ValueError("non-COPY operation graph contains a cycle")
        return order

    def topological_order(self, mode="locality"):
        if mode not in {"id", "locality", "critical"}:
            raise ValueError("unsupported topological order mode")
        return self._kahn(mode)


@dataclass
class Plan:
    blocks: dict
    core_of: dict
    order: list
    next_block_id: int
    ncores: int

    def clone(self):
        return Plan(dict(self.blocks), dict(self.core_of), list(self.order), self.next_block_id, self.ncores)

    @property
    def owner(self):
        return {v: b for b, nodes in self.blocks.items() for v in nodes}

    def to_dict(self):
        return {
            "node_to_subgraph": {str(v): b for b in sorted(self.blocks) for v in self.blocks[b]},
            "core_schedules": [[b for b in self.order if self.core_of[b] == k] for k in range(self.ncores)],
        }

    def fingerprint(self):
        # Stable IDs remain part of the identity because official COPY IDs can depend on them.
        value = json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(value.encode()).hexdigest()

    def validate(self, graph):
        if not isinstance(self.ncores, int) or self.ncores < 1:
            raise ValueError("ncores must be positive")
        if set(self.blocks) != set(self.core_of) or len(self.order) != len(self.blocks) or set(self.order) != set(self.blocks):
            raise ValueError("blocks, assignment and global order disagree")
        owner = self.owner
        if len(owner) != sum(map(len, self.blocks.values())) or set(owner) != graph.eligible:
            raise ValueError("plan must cover each eligible operation exactly once")
        for b, nodes in self.blocks.items():
            if not isinstance(b, int) or b < 0 or not nodes:
                raise ValueError("block IDs must be nonnegative and blocks nonempty")
            if not isinstance(self.core_of[b], int) or not 0 <= self.core_of[b] < self.ncores:
                raise ValueError("invalid core assignment")
            local_pos = {v: i for i, v in enumerate(nodes)}
            for v in nodes:
                if any(p in local_pos and local_pos[p] >= local_pos[v] for p in graph.preds[v]):
                    raise ValueError("block internal order violates a dependency")
        pos = {b: i for i, b in enumerate(self.order)}
        for v in graph.ids:
            for w in graph.succs[v]:
                if owner[v] != owner[w] and pos[owner[v]] >= pos[owner[w]]:
                    raise ValueError("global block order violates a dependency")
        if self.next_block_id <= max(self.blocks, default=-1):
            raise ValueError("next_block_id must be unused and greater than stable IDs")
        return True

    @classmethod
    def from_dict(cls, graph, data):
        mapping = {int(v): int(b) for v, b in data["node_to_subgraph"].items()}
        blocks = defaultdict(list)
        for v in graph.id_order:
            blocks[mapping[v]].append(v)
        core_of = {b: k for k, order in enumerate(data["core_schedules"]) for b in order}
        blocks = {b: tuple(nodes) for b, nodes in blocks.items()}
        preds, succs = block_dependencies(graph, blocks)
        # A supplied per-core list can constrain a valid interleaving further.
        for local in data["core_schedules"]:
            for a, b in zip(local, local[1:]):
                preds[b].add(a)
                succs[a].add(b)
        order = _topological_blocks(preds, succs)
        result = cls(blocks, core_of, order, max(blocks, default=-1) + 1, len(data["core_schedules"]))
        result.validate(graph)
        return result


def block_dependencies(graph, blocks):
    if isinstance(blocks, Plan):
        blocks = blocks.blocks
    owner = {v: b for b, nodes in blocks.items() for v in nodes}
    preds, succs = {b: set() for b in blocks}, {b: set() for b in blocks}
    for v in graph.ids:
        for w in graph.succs[v]:
            a, b = owner[v], owner[w]
            if a != b:
                preds[b].add(a)
                succs[a].add(b)
    return preds, succs


def _topological_blocks(preds, succs, priority=None):
    priority = priority or {b: b for b in preds}
    degree = {b: len(ps) for b, ps in preds.items()}
    heap = [(priority[b], b) for b in preds if not degree[b]]
    heapq.heapify(heap)
    order = []
    while heap:
        _, b = heapq.heappop(heap)
        order.append(b)
        for c in sorted(succs[b]):
            degree[c] -= 1
            if not degree[c]:
                heapq.heappush(heap, (priority[c], c))
    if len(order) != len(preds):
        raise ValueError("block dependency/order graph contains a cycle")
    return order


def build_blocks(graph, ncores, max_ops=64, deadline=None):
    """Step 1: contiguous locality-order intervals, closed on component changes."""
    target = sum(graph.weight.values()) / max(1, min(len(graph.ids), 8 * ncores))
    result, block, work, previous_component = {}, [], 0, None
    for v in graph.topological_order("locality"):
        check_deadline(deadline)
        component = graph.atom_of[v]
        if block and (len(block) >= max_ops or work + graph.weight[v] > target or component != previous_component):
            result[len(result)] = tuple(block)
            block, work = [], 0
        block.append(v)
        work += graph.weight[v]
        previous_component = component
    if block:
        result[len(result)] = tuple(block)
    return result


def single_core_plan(graph, ncores):
    blocks = {0: tuple(graph.id_order)} if graph.ids else {}
    return Plan(blocks, {b: 0 for b in blocks}, list(blocks), len(blocks), ncores)


def route_bytes(graph, plan, *, details=False):
    """Exact pre-spill COPY ledger for official scene B, including original I/O."""
    owner = plan.owner
    core = {v: plan.core_of[owner[v]] for v in graph.ids}
    ledger = []
    for tid in sorted(graph.tensors):
        size = graph.tensors[tid]["size"]
        ps = {core[v] for v in graph.eligible_producers[tid]}
        cs = {core[v] for v in graph.eligible_consumers[tid]}
        if cs and not ps:
            ledger.extend({"kind": "input", "tensor_id": tid, "target_core": c, "bytes": size} for c in sorted(cs))
        if tid in graph.original_output_relations:
            ledger.extend({"kind": "output", "tensor_id": tid, "source_core": p, "bytes": size} for p in sorted(ps))
        ledger.extend({"kind": "cross", "tensor_id": tid, "source_core": p, "target_core": c, "bytes": 2 * size}
                      for p in sorted(ps) for c in sorted(cs) if p != c)
    for index, edge in enumerate(graph.direct_edges):
        a, b = edge["source"], edge["target"]
        if a in core and b in core and core[a] != core[b]:
            ledger.append({"kind": "direct", "edge_index": index, "source_core": core[a],
                           "target_core": core[b], "bytes": 2 * max(0, int(edge.get("data_size", 0)))})
    total = sum(item["bytes"] for item in ledger)
    return {"total_ddr_bytes": total, "ledger": ledger} if details else total


def pipe_loads(graph, plan):
    loads = [{p: 0 for p in PIPES} for _ in range(plan.ncores)]
    for b, nodes in plan.blocks.items():
        for v in nodes:
            loads[plan.core_of[b]][graph.ops[v]["pipe"]] += graph.ops[v]["cycles"]
    return loads


class _Sketch:
    def __init__(self, graph, ncores, settings):
        self.graph, self.settings = graph, settings
        self.available = [{p: 0.0 for p in PIPES} for _ in range(ncores)]
        self.fixed = [{p: 0 for p in PIPES} for _ in range(ncores)]
        self.finish, self.core = {}, {}
        self.local_ready, self.copy_out, self.copy_in = {}, {}, {}
        self.output_done = set()
        self.route_bytes = 0

    def clone(self):
        other = _Sketch.__new__(_Sketch)
        other.graph, other.settings = self.graph, self.settings
        other.available = [dict(row) for row in self.available]
        other.fixed = [dict(row) for row in self.fixed]
        for name in ("finish", "core", "local_ready", "copy_out", "copy_in"):
            setattr(other, name, dict(getattr(self, name)))
        other.output_done = set(self.output_done)
        other.route_bytes = self.route_bytes
        return other

    def transfer(self, key, size, source, target, ready):
        """A distinct OUT/IN pair for each tensor source-target route."""
        duration = max(1, math.ceil(size / self.settings["bandwidth"]))
        if key not in self.copy_out:
            end = max(ready, self.available[source]["PIPE_MTE3"]) + duration
            self.available[source]["PIPE_MTE3"] = end
            self.copy_out[key] = end
            self.route_bytes += size
        if key not in self.copy_in:
            release = self.copy_out[key] + self.settings["cross_core_copy_delay"]
            end = max(release, self.available[target]["PIPE_MTE2"]) + duration
            self.available[target]["PIPE_MTE2"] = end
            self.copy_in[key] = end
            self.route_bytes += size
        return self.copy_in[key]

    def tensor_ready(self, tid, core):
        g = self.graph
        ps = g.eligible_producers[tid]
        size = g.tensors[tid]["size"]
        if not ps:
            key = (tid, core)
            if key not in self.local_ready:
                duration = max(1, math.ceil(size / self.settings["bandwidth"]))
                self.available[core]["PIPE_MTE2"] += duration
                self.local_ready[key] = self.available[core]["PIPE_MTE2"]
                self.route_bytes += size
            return self.local_ready[key]
        source_groups = defaultdict(list)
        for p in ps:
            source_groups[self.core[p]].append(p)
        ready = 0.0
        for source, nodes in source_groups.items():
            production = max(self.finish[v] for v in nodes)
            if source == core:
                ready = max(ready, production)
            else:
                ready = max(ready, self.transfer(("tensor", tid, source, core), size, source, core, production))
        self.local_ready[tid, core] = ready
        return ready

    def append(self, nodes, core):
        g = self.graph
        block_finish = 0.0
        for v in nodes:
            readiness = max((self.finish[p] for p in g.preds[v]), default=0.0)
            for tid in g.op_inputs[v]:
                readiness = max(readiness, self.tensor_ready(tid, core))
            for index, pred, size in g.direct_inputs[v]:
                if self.core[pred] != core:
                    readiness = max(readiness, self.transfer(("direct", index), size, self.core[pred], core, self.finish[pred]))
            op = g.ops[v]
            pipe = op["pipe"]
            # Official non-COPY execution clamps zero cycles, unlike the LB.
            end = max(readiness, self.available[core][pipe]) + max(1, op["cycles"])
            self.available[core][pipe] = end
            self.fixed[core][pipe] += op["cycles"]
            self.finish[v], self.core[v] = end, core
            block_finish = max(block_finish, end)
            for tid in g.op_outputs[v]:
                if tid not in g.original_output_relations:
                    continue
                producers = g.eligible_producers[tid]
                if not producers.issubset(self.finish):
                    continue
                groups = defaultdict(list)
                for p in producers:
                    groups[self.core[p]].append(p)
                for source, members in groups.items():
                    key = (tid, source)
                    if key in self.output_done:
                        continue
                    duration = max(1, math.ceil(g.tensors[tid]["size"] / self.settings["bandwidth"]))
                    tail = max(self.available[source]["PIPE_MTE3"], max(self.finish[p] for p in members)) + duration
                    self.available[source]["PIPE_MTE3"] = tail
                    self.output_done.add(key)
                    self.route_bytes += g.tensors[tid]["size"]
                    block_finish = max(block_finish, tail)
        return block_finish

    @property
    def makespan(self):
        return max((max(row.values()) for row in self.available), default=0.0)


def construct(graph, ncores, lambda_weight=0.0, settings=None, *, blocks=None, deadline=None):
    """Step 2: rank-priority native list placement with independent four pipes."""
    settings = _settings(settings)
    blocks = build_blocks(graph, ncores, deadline=deadline) if blocks is None else dict(blocks)
    preds, succs = block_dependencies(graph, blocks)
    topo = _topological_blocks(preds, succs)
    rank = {}
    for b in reversed(topo):
        rank[b] = sum(graph.weight[v] for v in blocks[b]) + max((rank[c] for c in succs[b]), default=0)
    order = _topological_blocks(preds, succs, {b: -rank[b] for b in blocks})
    sketch, assignment = _Sketch(graph, ncores, settings), {}
    for b in order:
        check_deadline(deadline)
        trials = []
        for core in range(ncores):
            trial = sketch.clone()
            finish = trial.append(blocks[b], core)
            delta = trial.route_bytes - sketch.route_bytes
            score = finish + lambda_weight * delta / settings["bandwidth"]
            trials.append(((score, max(trial.fixed[core].values()), core), trial))
        key, sketch = min(trials, key=lambda pair: pair[0])
        assignment[b] = key[2]
    result = Plan(blocks, assignment, order, max(blocks, default=-1) + 1, ncores)
    result.validate(graph)
    return result


def replay_sketch(graph, plan, settings=None, *, details=False, deadline=None):
    sketch = _Sketch(graph, plan.ncores, _settings(settings))
    for b in plan.order:
        check_deadline(deadline)
        sketch.append(plan.blocks[b], plan.core_of[b])
    if details:
        return {"makespan": sketch.makespan, "route_bytes": sketch.route_bytes,
                "available": sketch.available, "finish": sketch.finish}
    return sketch.makespan


def migrate(plan, blocks, target_core):
    result = plan.clone()
    if not 0 <= target_core < plan.ncores:
        raise ValueError("invalid migration target")
    for b in blocks:
        if b not in result.blocks:
            raise ValueError("unknown migration block")
        result.core_of[b] = target_core
    return result


def swap(plan, first, second):
    result = plan.clone()
    result.core_of[first], result.core_of[second] = result.core_of[second], result.core_of[first]
    return result


def split_block(graph, plan, block, cut=None):
    nodes = plan.blocks[block]
    if len(nodes) < 2:
        return None
    if cut is None:
        half = sum(graph.weight[v] for v in nodes) / 2
        total, cut = 0, 1
        for i, v in enumerate(nodes[:-1], 1):
            total += graph.weight[v]
            cut = i
            if total >= half:
                break
    if not 0 < cut < len(nodes):
        raise ValueError("split cut must be internal")
    result = plan.clone()
    new = result.next_block_id
    result.next_block_id += 1
    result.blocks[block], result.blocks[new] = nodes[:cut], nodes[cut:]
    result.core_of[new] = result.core_of[block]
    result.order.insert(result.order.index(block) + 1, new)
    result.validate(graph)
    return result


def merge_adjacent(graph, plan, first, second=None, max_ops=128):
    index = plan.order.index(first)
    if second is None:
        if index + 1 >= len(plan.order):
            return None
        second = plan.order[index + 1]
    if index + 1 >= len(plan.order) or plan.order[index + 1] != second:
        return None
    if plan.core_of[first] != plan.core_of[second] or len(plan.blocks[first]) + len(plan.blocks[second]) > max_ops:
        return None
    result = plan.clone()
    result.blocks[first] += result.blocks.pop(second)
    del result.core_of[second]
    result.order.remove(second)
    result.validate(graph)
    return result


def insertion_bounds(graph, plan, block):
    remaining = [b for b in plan.order if b != block]
    pos = {b: i for i, b in enumerate(remaining)}
    preds, succs = block_dependencies(graph, plan)
    lower = max((pos[b] + 1 for b in preds[block]), default=0)
    upper = min((pos[b] for b in succs[block]), default=len(remaining))
    return lower, upper


def move_block(graph, plan, block, gap):
    lower, upper = insertion_bounds(graph, plan, block)
    if not lower <= gap <= upper:
        return None
    result = plan.clone()
    result.order.remove(block)
    result.order.insert(gap, block)
    return result


def lifetime_proxy(graph, plan, capacity=None, *, details=False):
    """No-spill *linear* lifetime pressure; not physical concurrent occupancy.

    Includes external-input allocation at first use and COPY_OUT endpoints at
    their controlling producer blocks. Official expanded profiles supersede
    this approximation when locating current-plan hot tensors.
    """
    capacity = capacity or _settings()["capacity"]
    owner = plan.owner
    pos = {b: i for i, b in enumerate(plan.order)}
    local_pos, local_count = {}, Counter()
    for b in plan.order:
        core = plan.core_of[b]
        local_pos[b] = local_count[core]
        local_count[core] += 1
    intervals, spans = [], 0
    events = defaultdict(lambda: defaultdict(int))
    for tid, tensor in graph.tensors.items():
        prod, cons = defaultdict(set), defaultdict(set)
        for v in graph.eligible_producers[tid]:
            prod[plan.core_of[owner[v]]].add(owner[v])
        for v in graph.eligible_consumers[tid]:
            cons[plan.core_of[owner[v]]].add(owner[v])
        location = "UB" if tensor.get("pos") == "DDR" else tensor.get("pos")
        if location not in capacity:
            continue
        for core in sorted(set(prod) | set(cons)):
            pblocks, cblocks = prod[core], cons[core]
            source_uses = set(pblocks) if pblocks and (
                tid in graph.original_output_relations or any(k != core for k in cons)
            ) else set()
            # Multiple local writers remain distinct dependency endpoints.
            starts = pblocks or cblocks
            if not starts:
                continue
            first = min(pos[b] for b in starts)
            last = max((pos[b] for b in cblocks | source_uses | pblocks), default=first)
            first_local = local_pos[plan.order[first]]
            last_local = local_pos[plan.order[last]]
            span = tensor["size"] * (last_local - first_local + 1)
            spans += span
            events[core, location][first_local] += tensor["size"]
            events[core, location][last_local + 1] -= tensor["size"]
            intervals.append({"tensor_id": tid, "core_id": core, "position": location,
                              "size": tensor["size"], "first": first, "last": last,
                              "first_local": first_local, "last_local": last_local,
                              "span_bytes": span, "producer_blocks": sorted(pblocks),
                              "consumer_blocks": sorted(cblocks),
                              "last_blocks": sorted(b for b in cblocks | source_uses | pblocks if pos[b] == last)})
    peak, excess_area = {}, 0
    for key, sequence in events.items():
        live, last, maximum = 0, 0, 0
        for index, delta in sorted(sequence.items()):
            excess_area += max(0, live - capacity[key[1]]) * (index - last)
            live += delta
            maximum = max(maximum, live)
            last = index
        peak[key] = maximum
    score = (excess_area, spans)
    if details:
        return {"score": score, "excess_area": excess_area, "span_bytes": spans,
                "peak": peak, "intervals": sorted(intervals, key=lambda r: (-r["span_bytes"], r["tensor_id"], r["core_id"]))}
    return score


def repair_order(graph, plan, profile=None, affected_region=None, max_moves=2, settings=None):
    """Return at most one cheap lifetime repair; never accepts an official move.

    Uses at most eight tensor hotspots, early last-use / late-producer insertion
    and removal of one unrelated large-memory block. Endpoints and a same-core
    sixteen-block local window are sampled; no evaluator is called here.
    """
    settings = _settings(settings)
    current = plan
    for step in range(max_moves):
        life = lifetime_proxy(graph, current, settings["capacity"], details=True)
        intervals = life["intervals"]
        # Supplied official spill ranking, when available, selects physical tid
        # lineages. It does not replace candidate recomputation after a move.
        priorities = {}
        if isinstance(profile, dict):
            for record in profile.get("lifetime_hotspots", []):
                priorities[record.get("tensor_id"), record.get("core")] = record.get("spill_bytes", 0)
        if priorities:
            intervals = sorted(intervals, key=lambda r: (-priorities.get((r["tensor_id"], r["core_id"]), 0), -r["span_bytes"]))
        if step == 0 and isinstance(profile, dict) and profile.get("lifetime_hotspots"):
            # The supplied profile belongs to this exact plan. Retain the
            # official COPY endpoint, even when the last use is not movable.
            official_intervals, owner = [], current.owner
            order_pos = {b: i for i, b in enumerate(current.order)}
            for record in profile["lifetime_hotspots"]:
                controls = [b for b in record["producer_blocks"] + record["consumer_blocks"] if b in order_pos]
                if not controls:
                    continue
                pblocks = sorted({owner[v] for v in record.get("producer_ops", []) if v in owner})
                cblocks = sorted({owner[v] for v in record.get("consumer_ops", []) if v in owner})
                last = record.get("last_use_block")
                official_intervals.append({
                    "tensor_id": record["tensor_id"], "core_id": record["core"],
                    "position": record["pos"], "size": record["size"],
                    "first": min(order_pos[b] for b in controls),
                    "last": max(order_pos[b] for b in controls),
                    "span_bytes": record["size"] * (record["last_index"] - record["first_index"] + 1),
                    "producer_blocks": pblocks, "consumer_blocks": cblocks,
                    "last_blocks": [last] if last in order_pos and record.get("last_use_op") in graph.eligible else [],
                })
            intervals = official_intervals or intervals
        candidates, seen = [], set()
        base_sequence = tuple(current.order)
        for hotspot in intervals[:8]:
            if hotspot["first"] == hotspot["last"]:
                continue
            positions = {b: i for i, b in enumerate(current.order)}
            relevant = set(hotspot["producer_blocks"] + hotspot["consumer_blocks"])
            if affected_region is not None and relevant.isdisjoint(affected_region):
                continue
            targets = []
            for b in hotspot["last_blocks"]:
                if b in hotspot["consumer_blocks"]:
                    targets.append((b, "early"))
            targets.extend((b, "late") for b in hotspot["producer_blocks"])
            middle = [b for b in current.order[hotspot["first"] + 1:hotspot["last"]]
                      if current.core_of[b] == hotspot["core_id"] and b not in relevant]
            if middle:
                def footprint(b):
                    tids = {t for v in current.blocks[b] for t in graph.op_outputs[v]}
                    return sum(graph.tensors[t]["size"] for t in tids)
                targets.append((max(middle, key=lambda b: (footprint(b), -b)), "outside"))
            for block, direction in targets[:6]:
                lower, upper = insertion_bounds(graph, current, block)
                local = [b for b in current.order if current.core_of[b] == current.core_of[block]]
                local_index = local.index(block)
                left = positions[local[max(0, local_index - 16)]]
                right = positions[local[min(len(local) - 1, local_index + 16)]]
                if direction == "early":
                    gaps = {lower, max(lower, left), max(lower, hotspot["first"] + 1)}
                elif direction == "late":
                    first_use = min((positions[b] for b in hotspot["consumer_blocks"]), default=upper)
                    gaps = {upper, min(upper, right), min(upper, max(0, first_use - 1))}
                else:
                    gaps = {max(lower, hotspot["first"]), min(upper, hotspot["last"])}
                for gap in sorted(gaps):
                    if not lower <= gap <= upper:
                        continue
                    candidate = move_block(graph, current, block, gap)
                    key = tuple(candidate.order)
                    if key == base_sequence or key in seen:
                        continue
                    seen.add(key)
                    candidates.append(candidate)
                    if len(candidates) >= 24:
                        break
                if len(candidates) >= 24:
                    break
            if len(candidates) >= 24:
                break
        if not candidates:
            break
        # Pressure/span is a lossy ordering criterion; only three candidates
        # receive a complete four-pipe replay. Official acceptance is external.
        ranked = sorted(candidates, key=lambda p: lifetime_proxy(graph, p, settings["capacity"]))[:3]
        scored = []
        for candidate in ranked:
            proxy = max(replay_sketch(graph, candidate, settings), route_bytes(graph, candidate) / settings["bandwidth"])
            scored.append(((proxy, *lifetime_proxy(graph, candidate, settings["capacity"])), candidate))
        _, current = min(scored, key=lambda item: item[0])
        # Temporary proxy deterioration is permitted for the two-step repair.
        # The caller still retains and evaluates the original candidate.
    if current.fingerprint() == plan.fingerprint():
        return None
    current.validate(graph)
    return current
