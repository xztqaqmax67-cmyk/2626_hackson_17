"""Bounded, independent search routes; the global incumbent is only an output guard."""
from __future__ import annotations

from dataclasses import dataclass, field


def objective(result):
    return result['makespan'], result['added_copy_bytes']


def assignment_key(graph, plan):
    owner = plan.owner
    return tuple(plan.core_of[owner[v]] for v in graph.ids)


def seed_family(label):
    for prefix in ('structure_separator', 'structure_stage', 'structure_atoms', 'structure_dfs',
                   'pipe_', 'native_', 'activate_', 'inherited_', 'resumed_'):
        if label.startswith(prefix):
            return 'inherited' if prefix in ('inherited_', 'resumed_') else prefix.rstrip('_')
    return label


def diverse_rows(graph, rows, limit):
    """Keep one best representative per assignment before filling with order variants."""
    ordered = sorted(rows, key=lambda row: row[0])
    chosen, maps, fingerprints = [], set(), set()
    for distinct in (True, False):
        for row in ordered:
            key = row[1].fingerprint()
            if key in fingerprints:
                continue
            mapping = assignment_key(graph, row[1])
            if distinct and mapping in maps:
                continue
            chosen.append(row); maps.add(mapping); fingerprints.add(key)
            if len(chosen) == limit:
                return chosen
    return chosen


def polishing_seeds(graph, rows, limit=8):
    """Protect the best native seed, then cover construction families, then maps."""
    native = [row for row in rows if row[3].startswith(('structure_', 'pipe_', 'native_'))]
    ordered = sorted(rows, key=lambda row: row[0])
    chosen, used, families, maps = [], set(), set(), set()

    def add(row):
        key = row[1].fingerprint()
        if key in used or len(chosen) >= limit:
            return
        chosen.append(row); used.add(key)
        families.add(seed_family(row[3])); maps.add(assignment_key(graph, row[1]))

    if native:
        add(min(native, key=lambda row: row[0]))
    for row in ordered:
        if seed_family(row[3]) not in families:
            add(row)
    for row in ordered:
        if assignment_key(graph, row[1]) not in maps:
            add(row)
    return chosen


@dataclass
class Route:
    graph: object
    name: str
    rows: list
    reference: bool = False
    pool: dict = field(default_factory=dict)
    visited: set = field(default_factory=set)
    evaluated: set = field(default_factory=set)
    reordered: set = field(default_factory=set)
    activated: set = field(default_factory=set)
    repaired: set = field(default_factory=set)
    repair_failures: int = 0
    repair_opportunities: int = 0
    repair_cost: float = 0.
    seconds: float = 0.
    compact: bool = False

    def __post_init__(self):
        self.best = min(self.rows, key=lambda row: row[0])
        self.initial = self.best[0][0]
        self.stats = dict(name=self.name, reference=self.reference, initial_makespan=self.initial,
                          best_makespan=self.initial, local_improvements=0, steps=0, seconds=0.,
                          secondary_centers=0, fifo_calls=0, activation_calls=0,
                          repair_calls=0, repair_skipped=0, repair_seconds=0., status='pending')
        for row in self.rows:
            self.offer(row)
        # Release the seed list once the bounded frontier owns its representatives.
        self.rows = []

    def offer(self, row):
        self.evaluated.add(row[1].fingerprint())
        improvement = 0
        if row[0] < self.best[0]:
            improvement = 2 if row[0][0] < self.best[0][0] else 1
            self.best = row
            self.stats['local_improvements'] += 1
            self.stats['best_makespan'] = row[0][0]
        self.pool[row[1].fingerprint()] = row
        candidates = list(self.pool.values())
        selected = (sorted(candidates, key=lambda item: item[0])[:2 if self.compact else 4] if self.reference
                    else diverse_rows(self.graph, candidates, 3 if self.compact else 5))
        self.pool = {item[1].fingerprint(): item for item in selected}
        return improvement

    def next_center(self):
        candidates = sorted(self.pool.values(), key=lambda row: row[0])
        for row in candidates:
            key = row[1].fingerprint()
            if key in self.visited:
                continue
            self.visited.add(key)
            if row[0] > self.best[0]:
                if self.stats['secondary_centers'] >= (3 if self.compact else 16):
                    continue
                self.stats['secondary_centers'] += 1
            return row
        return None

    def repair_allowed(self, key):
        self.repair_opportunities += 1
        allowed = (key not in self.repaired
                   and self.repair_cost <= max(1., .18 * self.seconds)
                   and (self.repair_failures < 4 or self.repair_opportunities % 12 == 0))
        if not allowed:
            self.stats['repair_skipped'] += 1
        return allowed

    def weight(self):
        gain = max(0., (self.initial - self.best[0][0]) / max(1., self.initial))
        return (2. if self.reference else 1.) * (1. + min(1., 5. * gain))


def select_route(routes):
    """Time-weighted fair service; all unfinished routes retain positive weight."""
    return min(routes, key=lambda route: (route.seconds / route.weight(), route.name))
