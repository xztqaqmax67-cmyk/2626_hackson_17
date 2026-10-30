"""Input validation, deterministic ordering, and Step 1 DDR insertion.

COPY_IN/OUT in the input describe boundary transfers; compute placement causes
them to be regenerated per core. Internal non-DDR COPY ops keep their cycles.
"""
from collections import defaultdict, deque
from dataclasses import dataclass, field
import configparser
import heapq
import math

PIPES = ('PIPE_MTE2', 'PIPE_MTE3', 'PIPE_M', 'PIPE_V', 'PIPE_FIX')
COPY = {'COPY_IN', 'COPY_OUT'}


def integer(value, name, minimum=0):
    if type(value) is not int or value < minimum:
        raise ValueError(f'{name} must be an integer >= {minimum}: {value!r}')
    return value


def read_config(path):
    cfg = configparser.ConfigParser(inline_comment_prefixes=('#',))
    with open(path, encoding='utf-8-sig') as stream:
        cfg.read_file(stream)
    capacity = {p: cfg.getint('capacity', p) for p in ('L1', 'UB')}
    bandwidth = cfg.getfloat('bandwidth', 'bandwidth')
    delay = cfg.getfloat('multicore_scene_b', 'cross_core_copy_delay_cycles')
    if any(v <= 0 for v in capacity.values()):
        raise ValueError('capacities must be positive')
    if not math.isfinite(bandwidth) or bandwidth <= 0:
        raise ValueError('bandwidth must be finite and positive')
    if not math.isfinite(delay) or delay < 0:
        raise ValueError('sync delay must be finite and nonnegative')
    return capacity, bandwidth, delay


def topological(preds):
    succ = {n: [] for n in preds}
    degree = {n: len(p) for n, p in preds.items()}
    for n, parents in preds.items():
        for p in parents:
            if p not in succ:
                raise ValueError(f'unknown dependency {p} -> {n}')
            succ[p].append(n)
    ready = [n for n in preds if not degree[n]]
    heapq.heapify(ready)
    order = []
    while ready:
        n = heapq.heappop(ready)
        order.append(n)
        for s in succ[n]:
            degree[s] -= 1
            if not degree[s]:
                heapq.heappush(ready, s)
    if len(order) != len(preds):
        raise ValueError('dependency graph contains a cycle')
    return order, succ


def reverse_dfs(preds, names):
    """Fixed predecessor DFS, following the supplied old Step 1 tie breaks."""
    topo, succ = topological(preds)
    depth = {}
    for n in topo:
        depth[n] = max((depth[p] + 1 for p in preds[n]), default=0)
    key = lambda n: (names[n] != 'COPY_IN', depth[n], -n)
    roots = sorted((n for n in topo if not succ[n]),
                   key=lambda n: (names[n] != 'COPY_OUT', depth[n], -n))
    stack, seen, order = list(roots), set(), []
    while stack:
        n = stack[-1]
        if n in seen:
            stack.pop()
            continue
        parents = [p for p in preds[n] if p not in seen]
        if parents:
            stack.extend(sorted(parents, key=key))
        else:
            seen.add(n)
            order.append(n)
            stack.pop()
    return order


class Graph:
    def __init__(self, raw):
        if not isinstance(raw, dict) or any(not isinstance(raw.get(k), list)
                                           for k in ('ops', 'tensors', 'edges')):
            raise ValueError('graph must contain ops, tensors, edges lists')
        self.ops, self.tensors, ids = {}, {}, set()
        for kind, target in (('ops', self.ops), ('tensors', self.tensors)):
            for item in raw[kind]:
                n = integer(item['id'], 'id')
                if n in ids:
                    raise ValueError(f'duplicate node id {n}')
                ids.add(n)
                target[n] = dict(item)
                if kind == 'ops':
                    if item['pipe'] not in PIPES:
                        raise ValueError(f'unsupported pipe {item["pipe"]}')
                    if not isinstance(item['op'], str):
                        raise ValueError('op name must be a string')
                    integer(item['cycles'], 'cycles', -1 if item['op'] in COPY else 0)
                else:
                    integer(item['size'], 'tensor size')
                    if item['pos'] not in ('L1', 'UB', 'DDR'):
                        raise ValueError(f'unsupported tensor position {item["pos"]}')
        self.inputs = {n: set() for n in self.ops}
        self.outputs = {n: set() for n in self.ops}
        self.writers, self.readers = defaultdict(set), defaultdict(set)
        node_preds = {n: set() for n in ids}
        self.direct = []
        for e in raw['edges']:
            a, b = integer(e['source'], 'source'), integer(e['target'], 'target')
            if a not in ids or b not in ids:
                raise ValueError(f'unknown edge endpoint {a} -> {b}')
            if a in node_preds[b]:
                raise ValueError(f'duplicate edge {a} -> {b}')
            node_preds[b].add(a)
            if a in self.ops and b in self.tensors:
                self.outputs[a].add(b)
                self.writers[b].add(a)
            elif a in self.tensors and b in self.ops:
                self.inputs[b].add(a)
                self.readers[a].add(b)
            elif a in self.ops and b in self.ops:
                self.direct.append((a, b, integer(e.get('data_size', 0), 'data_size')))
            else:
                raise ValueError('tensor-to-tensor edge is unsupported')
        topological(node_preds)
        self.compute = {n for n, op in self.ops.items() if op['op'] not in COPY}
        self.preds = {n: set() for n in self.ops}
        for t in self.tensors:
            for r in self.readers[t]:
                self.preds[r].update(self.writers[t])
        for a, b, _ in self.direct:
            self.preds[b].add(a)
        topo, _ = topological(self.preds)
        nearest = {}
        self.compute_preds = {}
        for n in topo:
            ancestors = set().union(*(nearest[p] for p in self.preds[n])) if self.preds[n] else set()
            if n in self.compute:
                self.compute_preds[n] = ancestors
                nearest[n] = {n}
            else:
                nearest[n] = ancestors
        # Boundary regeneration is deliberately restricted to real boundary COPYs.
        # Reject an internal COPY bridge instead of silently inventing alias semantics.
        for n, op in self.ops.items():
            if op['op'] == 'COPY_IN':
                if nearest[n] or any(self.tensors[t]['pos'] != 'DDR' for t in self.inputs[n]):
                    raise ValueError(f'COPY_IN {n}: internal COPY bridges are not supported')
            if op['op'] == 'COPY_OUT':
                if any(self.readers[t] for t in self.outputs[n]):
                    raise ValueError(f'COPY_OUT {n}: internal COPY bridges are not supported')


def plan_view(graph, plan):
    if not isinstance(plan, dict) or set(plan) != {'node_to_subgraph', 'core_schedules'}:
        raise ValueError('plan requires exactly node_to_subgraph and core_schedules')
    raw = plan['node_to_subgraph']
    if not isinstance(raw, dict):
        raise ValueError('node_to_subgraph must be an object')
    mapping = {}
    for k, v in raw.items():
        if not isinstance(k, str) or not k.isascii() or not k.isdecimal():
            raise ValueError(f'invalid op id {k!r}')
        n = int(k)
        if n in mapping:
            raise ValueError('duplicate numeric op id')
        mapping[n] = integer(v, 'subgraph id')
    if set(mapping) != graph.compute:
        raise ValueError('plan must cover each non-COPY op exactly once')
    schedules = plan['core_schedules']
    if not isinstance(schedules, list) or not schedules or len(schedules) > 64:
        raise ValueError('core_schedules must contain 1..64 lists')
    owner, ranks = {}, {}
    for c, row in enumerate(schedules):
        if not isinstance(row, list):
            raise ValueError('core schedule must be a list')
        for i, s in enumerate(row):
            integer(s, 'scheduled subgraph')
            if s in owner:
                raise ValueError('subgraph scheduled more than once')
            owner[s], ranks[s] = c, i
    if set(owner) != set(mapping.values()):
        raise ValueError('schedules must cover every subgraph exactly once')
    core = {n: owner[s] for n, s in mapping.items()}
    rank = {n: ranks[s] for n, s in mapping.items()}
    contracted = {s: set() for s in owner}
    for n, parents in graph.compute_preds.items():
        for p in parents:
            if mapping[p] != mapping[n]:
                contracted[mapping[n]].add(mapping[p])
                if core[p] == core[n] and rank[p] >= rank[n]:
                    raise ValueError(f'core order violates dependency {p} -> {n}')
    topological(contracted)
    for t in graph.tensors:
        writers = graph.writers[t] & graph.compute
        if len({core[n] for n in writers}) > 1:
            raise ValueError(f'tensor {t}: writers on multiple cores require unspecified merge semantics')
    return core, rank, len(schedules)


@dataclass
class Node:
    id: int
    core: int
    name: str
    pipe: str
    cycles: float = 0
    size: int = 0
    ddr: bool = False
    inputs: set = field(default_factory=set)
    outputs: set = field(default_factory=set)
    deps: dict = field(default_factory=dict)
    rank: int = 0
    origin: int | None = None
    category: str = 'compute'

    def depend(self, other, delay=0):
        if other != self.id:
            self.deps[other] = max(self.deps.get(other, 0), delay)


class Program:
    def __init__(self):
        self.nodes, self.buffers = {}, {}
        self.next_id = 0
        self.cross = []

    def node(self, core, name, pipe, **kwargs):
        n = Node(self.next_id, core, name, pipe, **kwargs)
        self.next_id += 1
        self.nodes[n.id] = n
        return n


def build(graph, plan, delay, sync_mode='producer_consumer'):
    core, rank, ncores = plan_view(graph, plan)
    program = Program()
    original = {}
    for oid in sorted(graph.compute):
        op = graph.ops[oid]
        n = program.node(core[oid], op['op'], op['pipe'], cycles=op['cycles'],
                         rank=rank[oid], origin=oid)
        original[oid] = n
    for oid in graph.compute:
        for p in graph.compute_preds[oid]:
            if core[p] == core[oid]:
                original[oid].depend(original[p].id)
    tasks = [[] for _ in range(ncores)]

    def tensor(tid, size, pos, writers, readers, final):
        touched = sorted({core[n] for n in writers | readers})
        if not touched:
            return
        if pos == 'DDR':
            # Explicitly inherited from the old Scene-B input adapter.
            pos = 'UB'
        for c in touched:
            program.buffers[c, tid] = (pos, size)
        for n in writers:
            original[n].outputs.add((core[n], tid))
        for n in readers:
            original[n].inputs.add((core[n], tid))
        source = core[next(iter(writers))] if writers else None
        remote = sorted({core[n] for n in readers if core[n] != source})
        out = None
        if writers and (remote or final):
            out = program.node(source, 'COPY_OUT', 'PIPE_MTE3', size=size, ddr=True,
                               rank=max(rank[n] for n in writers), category='boundary')
            out.inputs.add((source, tid))
            for n in writers:
                out.depend(original[n].id)
        for c in remote:
            consumers = sorted(n for n in readers if core[n] == c)
            inp = program.node(c, 'COPY_IN', 'PIPE_MTE2', size=size, ddr=True,
                               rank=min(rank[n] for n in consumers), category='boundary')
            inp.outputs.add((c, tid))
            for n in consumers:
                original[n].depend(inp.id)
            if out:
                program.cross.append((out.id, inp.id, delay if sync_mode == 'copy_chain' else 0))
                if sync_mode == 'producer_consumer':
                    for p in sorted(writers):
                        for n in consumers:
                            program.cross.append((original[p].id, original[n].id, delay))

    for tid, t in sorted(graph.tensors.items()):
        writers = graph.writers[tid] & graph.compute
        readers = graph.readers[tid] & graph.compute
        final = bool(writers) and (not readers or any(graph.ops[n]['op'] == 'COPY_OUT'
                                                    for n in graph.readers[tid]))
        tensor(tid, t['size'], t['pos'], writers, readers, final)
    synthetic = max(graph.tensors.keys() | graph.ops.keys(), default=0) + 1
    covered = set()
    for a, b, size in graph.direct:
        if a not in graph.compute or b not in graph.compute or core[a] == core[b]:
            continue
        tensor(synthetic, size, 'UB', {a}, {b}, False)
        synthetic += 1
        covered.add((a, b))
    # Contracted control dependencies through excluded COPYs still impose sync.
    for b, parents in graph.compute_preds.items():
        for a in parents:
            if core[a] != core[b] and (a, b) not in covered:
                program.cross.append((original[a].id, original[b].id,
                                      delay if sync_mode == 'producer_consumer' else 0))
    for n in program.nodes.values():
        tasks[n.core].append(n.id)
    sequences = []
    for ids in tasks:
        preds = {n: set(program.nodes[n].deps) for n in ids}
        dfs = reverse_dfs(preds, {n: program.nodes[n].name for n in ids})
        seq = sorted(dfs, key=lambda n: program.nodes[n].rank)
        position = {n: i for i, n in enumerate(seq)}
        if any(position[p] >= position[n] for n, parents in preds.items() for p in parents):
            raise ValueError('subgraph order violates local data dependency')
        sequences.append(seq)
    return program, sequences
