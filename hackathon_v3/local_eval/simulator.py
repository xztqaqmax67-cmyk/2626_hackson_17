"""Event simulation with a shared DDR service clock and independent audits."""
from collections import defaultdict
import heapq
import math

EPS = 1e-8


def simulate(nodes, bandwidth, priority):
    succ = {n: [] for n in nodes}
    degree = {n: len(node.deps) for n, node in nodes.items()}
    ready_at = dict.fromkeys(nodes, 0.0)
    for n, node in nodes.items():
        for p, delay in node.deps.items():
            if p not in nodes:
                raise ValueError(f'unknown simulation predecessor {p}')
            succ[p].append((n, delay))
    delayed = [(0.0, n) for n, d in degree.items() if not d]
    heapq.heapify(delayed)
    queues = {(node.core, node.pipe): [] for node in nodes.values()}
    lanes = sorted(queues)
    busy, fixed, transfers, trace = {}, [], [], {}
    now = service = moved = 0.0
    done = set()
    pipe_orders = defaultdict(list)
    while len(done) < len(nodes):
        completed = []
        while fixed and fixed[0][0] <= now + EPS:
            _, n = heapq.heappop(fixed)
            completed.append(n)
        while transfers and transfers[0][0] <= service + EPS:
            _, n = heapq.heappop(transfers)
            completed.append(n)
        for n in sorted(completed):
            done.add(n)
            node = nodes[n]
            busy.pop((node.core, node.pipe))
            trace[n]['end'] = now
            trace[n]['service_end'] = service
            for s, delay in succ[n]:
                ready_at[s] = max(ready_at[s], now + delay)
                degree[s] -= 1
                if degree[s] == 0:
                    heapq.heappush(delayed, (ready_at[s], s))
        while delayed and delayed[0][0] <= now + EPS:
            _, n = heapq.heappop(delayed)
            node = nodes[n]
            heapq.heappush(queues[node.core, node.pipe], (priority[n], n))
        for lane in lanes:
            if lane in busy or not queues[lane]:
                continue
            _, n = heapq.heappop(queues[lane])
            node = nodes[n]
            busy[lane] = n
            pipe_orders[lane].append(n)
            trace[n] = {'start': now, 'service_start': service}
            if node.ddr:
                heapq.heappush(transfers, (service + node.size, n))
            else:
                heapq.heappush(fixed, (now + node.cycles, n))
        if len(done) == len(nodes):
            break
        ddr_time = (now + max(0.0, transfers[0][0] - service) * len(transfers) / bandwidth
                    if transfers else math.inf)
        next_time = min(fixed[0][0] if fixed else math.inf, ddr_time,
                        delayed[0][0] if delayed else math.inf)
        if not math.isfinite(next_time):
            blocked = [n for n in nodes if n not in trace][:12]
            raise ValueError(f'fixed pipe/memory/cross-core dependencies deadlock; blocked={blocked}')
        dt = max(0.0, next_time - now)
        if transfers:
            service += dt * bandwidth / len(transfers)
            moved += dt * bandwidth
            if next_time == ddr_time:
                # Snap to the analytically known completion threshold. Without
                # this, an ulp-sized residual at a large timestamp can produce
                # a zero-length next event forever.
                service = max(service, transfers[0][0])
        now = next_time
    return trace, dict(pipe_orders), moved


def audit(program, epochs, trace, pipe_orders, capacity, bandwidth, transferred):
    nodes = program.nodes
    if set(trace) != set(nodes):
        raise ValueError('audit: incomplete timeline')
    tol = 1e-5
    for n, node in nodes.items():
        item = trace[n]
        if item['end'] + tol < item['start']:
            raise ValueError('audit: negative duration')
        for p, delay in node.deps.items():
            if item['start'] + tol < trace[p]['end'] + delay:
                raise ValueError(f'audit: dependency/sync {p} -> {n}')
        if not node.ddr and abs(item['end'] - item['start'] - node.cycles) > tol:
            raise ValueError('audit: compute duration differs from cycles')
    for lane, order in pipe_orders.items():
        for a, b in zip(order, order[1:]):
            if trace[a]['end'] > trace[b]['start'] + tol:
                raise ValueError(f'audit: overlapping or reordered pipe {lane}')
    memory_events = defaultdict(list)
    for e in epochs:
        pos, size = program.buffers[e.buffer]
        start = trace[e.alloc]['start']
        end = max(trace[n]['end'] for n in e.uses)
        if any(trace[n]['start'] + tol < start for n in e.uses):
            raise ValueError('audit: use before allocation')
        if end > start:
            memory_events[e.buffer[0], pos].extend([(start, size), (end, -size)])
    peaks = {}
    for (core, pos), events in memory_events.items():
        current = peak = 0
        for _, delta in sorted(events, key=lambda x: (x[0], x[1])):
            current += delta
            peak = max(peak, current)
            if current < 0 or current > capacity[pos]:
                raise ValueError(f'audit: core {core} {pos} occupancy {current}')
        if current:
            raise ValueError('audit: leaked tensor allocation')
        peaks.setdefault(str(core), {})[pos] = peak
    # Independent sweep of start/end events reconstructs the fair-share integral
    # for EVERY transfer, not only aggregate bytes / makespan.
    changes = defaultdict(list)
    for n, node in nodes.items():
        if node.ddr and node.size:
            changes[trace[n]['start']].append((1, n))
            changes[trace[n]['end']].append((-1, n))
    active, start_service = set(), {}
    service = prev = integral = 0.0
    for when, events in sorted(changes.items()):
        if active:
            dt = when - prev
            service += dt * bandwidth / len(active)
            integral += dt * bandwidth
        for sign, n in sorted(events):
            if sign < 0:
                actual = service - start_service[n]
                if abs(actual - nodes[n].size) > max(tol, nodes[n].size * 1e-9):
                    raise ValueError(f'audit: DDR bytes mismatch at op {n}: {actual}')
                active.remove(n)
            else:
                active.add(n)
                start_service[n] = service
        prev = when
    expected = sum(n.size for n in nodes.values() if n.ddr)
    if active or abs(integral - expected) > max(tol, expected * 1e-9):
        raise ValueError('audit: DDR byte conservation')
    if abs(transferred - expected) > max(tol, expected * 1e-9):
        raise ValueError('audit: simulator transfer accounting')
    return {'audit_passed': True, 'peak_memory_bytes': peaks,
            'checks': ['data_dependencies', 'cross_core_sync', 'fixed_pipe_order',
                       'compute_cycles', 'memory_capacity', 'per_transfer_fair_bandwidth',
                       'DDR_byte_conservation']}
