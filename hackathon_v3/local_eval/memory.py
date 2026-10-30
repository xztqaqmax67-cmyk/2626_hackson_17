"""Step 2: farthest-next-use spilling and explicit memory reuse dependencies.

Virtual byte ranges are reused only after all users of their previous allocation
finish. Tensor epochs preserve both readers and later writers across a spill.
These deterministic dependencies can be conservative relative to other legal
Step 2 implementations; they are recorded as a local-model scheduling choice.
"""
from collections import defaultdict, deque
from dataclasses import dataclass, field
import heapq


@dataclass
class Epoch:
    buffer: tuple
    alloc: int
    uses: set = field(default_factory=set)
    frontier: set = field(default_factory=set)
    writer: int | None = None


class Arena:
    def __init__(self, capacity):
        self.free = deque([(capacity, frozenset())])
        self.available = capacity

    def allocate(self, size, node):
        if size > self.available:
            raise ValueError('internal memory reservation overflow')
        self.available -= size
        while size:
            n, blockers = self.free.popleft()
            taken = min(n, size)
            for p in blockers:
                node.depend(p)
            size -= taken
            if taken < n:
                self.free.appendleft((n - taken, blockers))

    def release(self, size, blockers):
        if size:
            self.free.append((size, frozenset(blockers)))
            self.available += size


def insert_spills(program, sequences, capacity):
    epochs, expanded = [], []
    for c, sequence in enumerate(sequences):
        arenas = {p: Arena(size) for p, size in capacity.items()}
        future = defaultdict(deque)
        for i, nid in enumerate(sequence):
            node = program.nodes[nid]
            for t in node.inputs | node.outputs:
                future[t].append(i)
        resident, seen, backing = {}, set(), {}
        heaps = {p: [] for p in capacity}
        ext = []

        def allocate(t, node):
            pos, size = program.buffers[t]
            arenas[pos].allocate(size, node)
            e = Epoch(t, node.id)
            epochs.append(e)
            resident[t] = e
            seen.add(t)
            return e

        def access(t, node, writing=False):
            e = resident[t]
            node.depend(e.alloc)
            if e.writer is not None:
                node.depend(e.writer)
            if writing:
                for p in e.frontier:
                    node.depend(p)
                e.frontier = {node.id}
                e.writer = node.id
            else:
                e.frontier.add(node.id)
            e.uses.add(node.id)

        def release(t):
            e = resident.pop(t)
            pos, size = program.buffers[t]
            arenas[pos].release(size, e.frontier)

        def evict(t):
            # A later partial write still needs the previous contents restored.
            if t not in backing:
                pos, size = program.buffers[t]
                out = program.node(c, 'COPY_OUT', 'PIPE_MTE3', size=size,
                                   ddr=True, inputs={t}, category='spill_out')
                access(t, out)
                backing[t] = out.id
                ext.append(out.id)
            release(t)

        for i, nid in enumerate(sequence):
            node = program.nodes[nid]
            needed = node.inputs | node.outputs
            demand = defaultdict(int)
            for t in needed:
                pos, size = program.buffers[t]
                demand[pos] += size
            for pos, amount in demand.items():
                if amount > capacity[pos]:
                    raise ValueError(f'core {c} op {node.origin or nid}: simultaneous {pos} '
                                     f'operands require {amount} bytes, capacity={capacity[pos]}')
            for t in sorted(needed):
                if t in resident:
                    continue
                pos, size = program.buffers[t]
                held = []
                while arenas[pos].available < size:
                    victim = None
                    while heaps[pos]:
                        neg_next, candidate = heapq.heappop(heaps[pos])
                        if candidate not in resident or not future[candidate]:
                            continue
                        if -neg_next != future[candidate][0]:
                            continue
                        if candidate in needed:
                            held.append((neg_next, candidate))
                            continue
                        victim = candidate
                        break
                    if victim is None:
                        raise ValueError('memory allocation has no legal spill victim')
                    evict(victim)
                for item in held:
                    heapq.heappush(heaps[pos], item)
                if t in seen:
                    if t not in backing:
                        raise ValueError('missing backing for a spilled live tensor')
                    inp = program.node(c, 'COPY_IN', 'PIPE_MTE2', size=size, ddr=True,
                                       outputs={t}, category='spill_in')
                    inp.depend(backing[t])
                    allocate(t, inp)
                    access(t, inp, writing=True)
                    ext.append(inp.id)
                else:
                    if t in node.inputs:
                        raise ValueError(f'uninitialized local tensor {t}')
                    allocate(t, node)
            for t in sorted(needed):
                access(t, node, t in node.outputs)
                if t in node.outputs:
                    if node.name == 'COPY_IN' and node.ddr:
                        # Original DDR backing remains valid until a compute write.
                        backing[t] = node.id
                    else:
                        backing.pop(t, None)
                if node.name == 'COPY_OUT' and node.ddr and t in node.inputs:
                    backing[t] = node.id
                future[t].popleft()
            ext.append(nid)
            for t in sorted(needed):
                if not future[t]:
                    release(t)
                else:
                    pos, _ = program.buffers[t]
                    heapq.heappush(heaps[pos], (-future[t][0], t))
        expanded.append(ext)
    return epochs, expanded
