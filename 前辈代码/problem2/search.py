"""Scene-B portfolio construction and bounded, fair multi-neighborhood search."""
from __future__ import annotations
from collections import defaultdict, OrderedDict
from dataclasses import dataclass
import json
import math
from pathlib import Path
import signal
import time

from .submission_io import safe_message
from .utils import atomic_write
from . import core, official
from .portfolio import Route, assignment_key, diverse_rows, objective, polishing_seeds, select_route


class Deadline(BaseException):
    pass


class EvaluationLimit(Exception):
    pass


@dataclass
class Candidate:
    kind: str
    label: str
    plan: object
    needs_repair: bool = False


def block_loads(graph, plan):
    blocks = {}
    cores = [defaultdict(float) for _ in range(plan.ncores)]
    for b, ops in plan.blocks.items():
        load = defaultdict(float)
        for v in ops:
            # A safe bound uses original non-COPY cycles, including genuine zeros.
            load[graph.ops[v]['pipe']] += graph.ops[v]['cycles']
        blocks[b] = load
        for pipe, work in load.items():
            cores[plan.core_of[b]][pipe] += work
    return blocks, cores


def pipe_bound(graph, plan):
    return max((v for row in block_loads(graph, plan)[1] for v in row.values()), default=0)


def moved(plan, blocks, target):
    candidate = plan.clone()
    for b in blocks:
        candidate.core_of[b] = target
    return candidate


def split(plan, block, cut):
    ops = plan.blocks[block]
    if not 0 < cut < len(ops):
        return None
    candidate = plan.clone()
    new = candidate.next_block_id
    candidate.next_block_id += 1
    candidate.blocks[block] = tuple(ops[:cut])
    candidate.blocks[new] = tuple(ops[cut:])
    candidate.core_of[new] = candidate.core_of[block]
    candidate.order.insert(candidate.order.index(block) + 1, new)
    return candidate


def insert(graph, plan, block, desired):
    """Insert in the direct predecessor/successor gap; no global barriers."""
    candidate = plan.clone()
    candidate.order.remove(block)
    pos = {b: i for i, b in enumerate(candidate.order)}
    owner = {v: b for b, ops in plan.blocks.items() for v in ops}
    before = {owner[v] for op in plan.blocks[block] for v in graph.preds[op]} - {block}
    after = {owner[v] for op in plan.blocks[block] for v in graph.succs[op]} - {block}
    lo = max((pos[b] + 1 for b in before), default=0)
    hi = min((pos[b] for b in after), default=len(candidate.order))
    candidate.order.insert(max(lo, min(hi, desired)), block)
    return candidate


def tensor_groups(graph, plan):
    owner = {v: b for b, ops in plan.blocks.items() for v in ops}
    rows = []
    for tid, tensor in graph.tensors.items():
        producers = {owner[v] for v in graph.eligible_producers[tid]}
        consumers = {owner[v] for v in graph.eligible_consumers[tid]}
        if not consumers:
            continue
        grouped = defaultdict(set)
        for b in consumers:
            grouped[plan.core_of[b]].add(b)
        sources = {plan.core_of[b] for b in producers}
        pairs = sum(s != k for s in sources for k in grouped)
        traffic = tensor['size'] * (2 * pairs if producers else max(0, len(grouped) - 1))
        rows.append((traffic, tid, producers, dict(grouped)))
    return sorted(rows, key=lambda r: (-r[0], r[1]))


def neighborhoods(graph, plan, profile, official_result, family, no_group=False, no_repair=False, settings=None):
    """At most 48 candidates; alternate subtypes to preserve coverage."""
    pools = defaultdict(list)
    blocks, loads = block_loads(graph, plan)
    positions = {b: i for i, b in enumerate(plan.order)}
    op_block = {v: b for b, ops in plan.blocks.items() for v in ops}

    def add(kind, label, candidate, repair=False):
        if candidate is not None:
            # Store factories for potentially large migration pools. Only the
            # selected round-robin prefix creates complete Plan objects.
            pools[kind].append((kind, label, candidate, repair))

    def exchange(group, target, other, source):
        candidate = moved(plan, group, target)
        candidate.core_of[other] = source
        return candidate

    heavy = sorted(plan.blocks, key=lambda b: (-max(blocks[b].values(), default=0), b))[:4]
    if family == 0:
        if not no_repair:
            add('order', 'lifetime_repair', core.repair_order(graph, plan, profile=profile, settings=settings))
            for hotspot in profile.get('lifetime_hotspots', [])[:8]:
                cuts = []
                for op in hotspot.get('producer_ops', []):
                    if op in op_block:
                        b = op_block[op]
                        cuts.append((b, plan.blocks[b].index(op) + 1))
                op = hotspot.get('last_use_op')
                if op in op_block:
                    b = op_block[op]
                    cuts.append((b, plan.blocks[b].index(op)))
                for b, cut in cuts[:2]:
                    add('split', f'hotspot_split_{b}_{cut}', split(plan, b, cut), True)
        for b in heavy:
            if len(plan.blocks[b]) > 1:
                total = sum(graph.weight[v] for v in plan.blocks[b])
                partial, cut = 0, 1
                for i, v in enumerate(plan.blocks[b][:-1], 1):
                    partial += graph.weight[v]
                    cut = i
                    if partial >= total / 2:
                        break
                divided = split(plan, b, cut)
                add('split', f'split_{b}', divided)
                pipe = max(blocks[b], key=blocks[b].get)
                targets = sorted(range(plan.ncores), key=lambda k: (loads[k][pipe], k))
                for k in [k for k in targets if k != plan.core_of[b]][:2]:
                    add('split', f'split_migrate_{b}_{k}', moved(divided, [divided.next_block_id - 1], k), True)
        # Only globally adjacent blocks may be merged without an extra cycle proof.
        for a, b in zip(plan.order, plan.order[1:]):
            if plan.core_of[a] == plan.core_of[b] and len(plan.blocks[a]) + len(plan.blocks[b]) <= 128:
                merged = plan.clone()
                merged.blocks[a] += merged.blocks.pop(b)
                del merged.core_of[b]
                merged.order.remove(b)
                add('merge', f'merge_{a}_{b}', merged)
                if len(pools['merge']) == 8:
                    break
    elif family == 1:
        for traffic, tid, producers, groups in tensor_groups(graph, plan)[:8]:
            if not traffic:
                continue
            targets = sorted({plan.core_of[b] for b in producers} | set(groups))
            for source, group in sorted(groups.items()):
                for target in targets:
                    if source == target:
                        continue
                    group = sorted(group)
                    if not no_group and len(group) > 1:
                        add('tensor', f'group_t{tid}_{source}_{target}', lambda group=group, target=target: moved(plan, group, target), True)
                    for b in group[:2]:
                        add('tensor', f'consumer_t{tid}_{b}_{target}', lambda b=b, target=target: moved(plan, [b], target), True)
                    # A complete exchange can offset an otherwise overloaded destination.
                    moving = group if not no_group else group[:1]
                    work = sum(sum(blocks[b].values()) for b in moving)
                    compensators = sorted((b for b in plan.blocks if plan.core_of[b] == target and b not in producers),
                                          key=lambda b: (abs(sum(blocks[b].values()) - work), b))[:2]
                    for other in compensators:
                        add('swap', f'exchange_t{tid}_{other}',
                            lambda moving=moving, target=target, other=other, source=source: exchange(moving, target, other, source), True)
            for b in sorted(producers):
                for target in sorted(groups):
                    if plan.core_of[b] != target:
                        add('tensor', f'producer_t{tid}_{b}_{target}', lambda b=b, target=target: moved(plan, [b], target), True)
        busiest = max(((work, k, pipe) for k, row in enumerate(loads) for pipe, work in row.items()), default=(0, 0, 'PIPE_M'))
        _, source, pipe = busiest
        heavy_here = sorted((b for b in plan.blocks if plan.core_of[b] == source), key=lambda b: (-blocks[b][pipe], b))[:4]
        targets = sorted((k for k in range(plan.ncores) if k != source), key=lambda k: (loads[k][pipe], k))[:2]
        for b in heavy_here:
            for k in targets:
                add('load', f'load_{b}_{k}', lambda b=b, k=k: moved(plan, [b], k), True)
        # Relocate a short connected run as one candidate, so its internal
        # communication is not penalized as a succession of isolated moves.
        _, successors = core.block_dependencies(graph, plan)
        for b in heavy_here:
            group, tail = [b], b
            for _ in range(5):
                choices = [c for c in successors[tail]
                           if plan.core_of[c] == source and c not in group]
                if not choices:
                    break
                tail = max(choices, key=lambda c: (blocks[c][pipe], -c))
                group.append(tail)
            if len(group) > 1:
                for k in targets:
                    add('load_group', f'load_chain_{b}_{k}',
                        lambda group=tuple(group), k=k: moved(plan, group, k), True)
    else:
        # A late queue head can block later independent work even with zero
        # spills. Propose legal block insertions around observed Pipe gaps.
        gaps = []
        for timeline in official_result.get('per_core_timeline', []):
            previous = defaultdict(float)
            for op in sorted(timeline['ops'], key=lambda op: (op['start'], op['op_id'])):
                b = op.get('subgraph_id')
                gap = op['start'] - previous[op['pipe']]
                if gap > 0 and b in plan.blocks:
                    gaps.append((gap, b, op['pipe']))
                previous[op['pipe']] = max(previous[op['pipe']], op['end'])
        for _, blocked, pipe in sorted(gaps, reverse=True)[:8]:
            later = [b for b in plan.order[positions[blocked] + 1:]
                     if plan.core_of[b] == plan.core_of[blocked] and blocks[b][pipe] > 0][:8]
            for b in later[:2]:
                add('fifo', f'fifo_{blocked}_{b}',
                    lambda b=b, blocked=blocked: insert(graph, plan, b, positions[blocked]))
        # Observed late transfers are a near-critical heuristic, not an exact critical path.
        transfers = sorted(official_result.get('cross_core_transfers', []),
                           key=lambda r: (-r['copy_in_end'], -r['size'], r['tensor_id']))[:8]
        for transfer in transfers:
            tid = transfer['tensor_id']
            if tid not in graph.tensors:
                continue
            producers = sorted({op_block[v] for v in graph.eligible_producers[tid]
                                if plan.core_of[op_block[v]] == transfer['source_core']})
            consumers = sorted({op_block[v] for v in graph.eligible_consumers[tid]
                                if plan.core_of[op_block[v]] == transfer['target_core']})
            if not producers or not consumers:
                continue
            a = max(producers, key=positions.get)
            b = min(consumers, key=positions.get)
            chain = [a, b]
            tail = b
            for _ in range(2):
                successors = {op_block[v] for op in plan.blocks[tail] for v in graph.succs[op]} - set(chain)
                if not successors:
                    break
                tail = max(successors, key=lambda c: (max(graph.rank[v] for v in plan.blocks[c]), -c))
                chain.append(tail)
            targets = sorted({plan.core_of[c] for c in chain})
            for k in targets:
                add('chain', f'chain_t{tid}_{k}', lambda chain=chain, k=k: moved(plan, chain, k), True)
                add('chain', f'pair_t{tid}_{k}', lambda chain=chain, k=k: moved(plan, chain[:2], k), True)
            add('release', f'early_source_t{tid}', lambda a=a: insert(graph, plan, a, 0))
            # Move a few high-traffic unrelated blocks after the consumer, within legal gaps.
            candidates = [c for c in plan.order[positions[a] + 1:positions[b]] if c not in chain]
            def io_work(c):
                tids = {t for v in plan.blocks[c] for t in graph.op_inputs[v] + graph.op_outputs[v]}
                return sum(graph.tensors[t]['size'] for t in tids)
            for c in sorted(candidates, key=lambda c: (-io_work(c), c))[:2]:
                add('bandwidth', f'bandwidth_t{tid}_{c}', lambda c=c, b=b: insert(graph, plan, c, positions[b]))
    result = []
    index = 0
    while len(result) < 48:
        changed = False
        for pool in pools.values():
            if index < len(pool):
                kind, label, candidate, repair = pool[index]
                candidate = candidate() if callable(candidate) else candidate
                if candidate is not None:
                    result.append(Candidate(kind, label, candidate, repair))
                changed = True
                if len(result) == 48:
                    break
        if not changed:
            break
        index += 1
    return result


def solve(graph_path, ncores, config_path, output, time_limit=1200, warm_start=None,
          ablation='full', max_evaluations=0, inherited_checkpoint=None, resume_checkpoint=None,
          feasibility_grace=0., compact=False, export_results=True):
    started = time.monotonic()
    if not math.isfinite(time_limit) or time_limit <= 0 or not 0 <= feasibility_grace <= 120:
        raise ValueError('invalid search target or feasibility grace')
    feasibility_grace = 0. if ncores == 1 else feasibility_grace
    soft_deadline = started + time_limit
    deadline = soft_deadline + feasibility_grace
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    stats = dict(full_evaluations=0, invalid_evaluations=0, timeout_evaluations=0, expansions=0, invalid_expansions=0,
                 generated=0, duplicates=0, safe_pruned=0, accepted=0, batches=0,
                 primary_improvements=0, secondary_improvements=0, seed_candidates=0,
                 elite_restarts=0, candidate_pages=0,
                 construction_seconds=0., screening_seconds=0., expansion_seconds=0., evaluation_seconds=0.,
                 generation_seconds=0., repair_seconds=0., validation_seconds=0.,
                 record_seconds=0., checkpoint_seconds=0., coupled_reorders=0,
                 coupled_reorder_seconds=0., secondary_centers_expanded=0,
                 seed_reorders=0, seed_reorder_seconds=0., local_improvements=0,
                 route_steps=0, repair_skipped=0, activation_rounds=0)
    record = dict(status='running', solver_version='scene-b-v5-case-budget', time_limit=time_limit, ablation=ablation,
                  feasibility_grace_seconds=feasibility_grace, search_profile='compact' if compact else 'full',
                  warm_start=Path(warm_start).name if warm_start else None, stats=stats, history=[])
    best_plan = best_result = None
    best_key = (float('inf'), float('inf'))
    evaluated, profile_cache = {}, {}
    durations = []
    proxy_cache, result_cache = {}, OrderedDict()
    last_evaluation_result = None
    current_route = None
    last_saved = -float('inf')

    def alarm(signum, frame):
        raise Deadline()

    def check(reserve=False):
        remaining = deadline - time.monotonic()
        if remaining <= (max(1., 1.25 * max(durations[-3:], default=0.)) if reserve else 0):
            raise Deadline()
        if max_evaluations and stats['full_evaluations'] >= max_evaluations:
            raise EvaluationLimit('evaluation_limit')

    def save_record(force=False):
        nonlocal last_saved
        began = time.monotonic()
        if compact and not force and began - last_saved < 2.:
            return
        last_saved = began
        record.update(time_seconds=time.monotonic() - started)
        atomic_write(output / 'metrics.json', record)
        stats['record_seconds'] += time.monotonic() - began

    previous_alarm = signal.signal(signal.SIGALRM, alarm)
    signal.setitimer(signal.ITIMER_REAL, max(.001, deadline - time.monotonic()))
    try:
        save_record()
        graph = core.GraphInfo.from_path(graph_path)
        settings = official.load_settings(config_path)
        from .inheritance import instance_fingerprint, load_inherited
        instance = instance_fingerprint(graph_path, config_path)
        record['problem_instance'] = instance

        def evaluate(candidate, source, expected_objective=None):
            nonlocal best_key, best_plan, best_result, last_evaluation_result, deadline
            check()
            validation_started = time.monotonic()
            candidate.validate(graph)
            key = candidate.fingerprint()
            stats['validation_seconds'] += time.monotonic() - validation_started
            if key in evaluated and (key in result_cache or evaluated[key] != 'ok'):
                stats['duplicates'] += 1
                last_evaluation_result = result_cache.get(key)
                return 0
            began = time.monotonic()
            stats['full_evaluations'] += 1
            result = official.evaluate(graph.graph, candidate.to_dict(), settings)
            last_evaluation_result = result
            elapsed = time.monotonic() - began
            durations.append(elapsed); stats['evaluation_seconds'] += elapsed
            evaluated[key] = result['status']
            result_cache[key] = result
            while len(result_cache) > (12 if compact else 64):
                result_cache.popitem(last=False)
            entry = dict(source=source, status=result['status'], seconds=elapsed,
                         elapsed_seconds=time.monotonic() - started,
                         route=current_route.name if current_route else 'construction')
            improved = 0
            if result['status'] == 'ok':
                objective = (result['makespan'], result['added_copy_bytes'])
                if expected_objective is not None:
                    if objective != expected_objective:
                        record['inherited_validation'].update(status='failed', revalidated_makespan=objective[0])
                        raise ValueError('idle-core embedding changed official objective')
                    record['inherited_validation'].update(status='verified',
                        revalidated_makespan=objective[0], revalidated_added_copy_bytes=objective[1])
                entry.update(makespan=objective[0], added_copy_bytes=objective[1])
                if objective < best_key:
                    improved = 2 if objective[0] < best_key[0] else 1
                    checkpoint = dict(plan=candidate.to_dict(), official_result=result['official_result'],
                                      makespan=result['makespan'], source=source,
                                      elapsed_seconds=time.monotonic() - started,
                                      problem_instance=instance,
                                      inherited_validation=record.get('inherited_validation'))
                    checkpoint_started = time.monotonic()
                    atomic_write(output / 'best_checkpoint.json', checkpoint)
                    stats['checkpoint_seconds'] += time.monotonic() - checkpoint_started
                    # Commit only after the recoverable checkpoint exists.
                    first = best_plan is None
                    best_key, best_plan, best_result = objective, candidate, result
                    if first:
                        record['first_solution_seconds'] = time.monotonic() - started
                        record['feasibility_extension_used'] = time.monotonic() > soft_deadline
                        deadline = soft_deadline
                        # Disable the grace timer before final metadata if the
                        # first checkpoint arrived late. No more evaluations.
                        signal.setitimer(signal.ITIMER_REAL,
                            max(.001, deadline - time.monotonic()) if deadline > time.monotonic() else 0.)
                    stats['primary_improvements' if improved == 2 else 'secondary_improvements'] += 1
                    stats['accepted'] += 1
                    record.update(makespan=result['makespan'], source=source, solution_available=True,
                                  active_cores=len(set(candidate.core_of.values())),
                                  data_movement_bytes=result['official_result']['data_movement_bytes'])
            else:
                stats['timeout_evaluations' if result['status'] == 'timeout' else 'invalid_evaluations'] += 1
                entry['error'] = result.get('error')
            record['history'].append(entry)
            save_record(force=bool(improved))
            print(f"{source}: {entry.get('makespan', entry['status'])}; best={best_key[0]}", flush=True)
            if best_plan is not None and time.monotonic() >= soft_deadline:
                raise Deadline()
            return improved

        def expand(candidate, result=None):
            check(reserve=True)
            key = candidate.fingerprint()
            if key in profile_cache:
                return profile_cache[key]
            began = time.monotonic()
            stats['expansions'] += 1
            try:
                profile = official.build_profile(graph.graph, candidate.to_dict(), settings, result=result)
            except official.INVALID_ERRORS as exc:
                profile = {'status': 'invalid', 'error': safe_message(exc)}
            stats['expansion_seconds'] += time.monotonic() - began
            profile_cache[key] = profile
            if profile.get('status', 'ok') != 'ok':
                stats['invalid_expansions'] += 1
            return profile

        def cheap(candidate):
            key = candidate.fingerprint()
            if key in proxy_cache:
                return proxy_cache[key]
            estimate = core.replay_sketch(graph, candidate, settings)
            if isinstance(estimate, dict):
                estimate = estimate['makespan']
            route = core.route_bytes(graph, candidate)
            score = max(float(estimate), route / settings['bandwidth']), route
            proxy_cache[key] = score
            return score

        # The inherited plan is the FIRST official evaluation inside this job's
        # deadline. It is never replaced by an unverified source objective.
        fallback = core.Plan({0: tuple(graph.id_order)}, {0: 0}, [0], 1, ncores)
        inherited_plan = None
        if inherited_checkpoint is not None or resume_checkpoint is not None:
            if inherited_checkpoint is not None and resume_checkpoint is not None:
                raise ValueError('choose an inherited checkpoint or a same-core resume checkpoint')
            inherited_plan, metadata = load_inherited(
                graph, resume_checkpoint or inherited_checkpoint, ncores, instance,
                allow_same_core=resume_checkpoint is not None)
            if resume_checkpoint is not None and metadata['source_cores'] != ncores:
                raise ValueError('resume checkpoint must have the same core count')
            metadata['mode'] = 'resume' if resume_checkpoint is not None else 'inherit'
            record['inherited_validation'] = metadata
            label = f'resumed_{ncores}' if resume_checkpoint is not None else f'inherited_{metadata["source_cores"]}_to_{ncores}'
            evaluate(inherited_plan, label,
                     expected_objective=(metadata['source_makespan'], metadata['source_added_copy_bytes']))
            if best_plan is None:
                record['inherited_validation']['status'] = 'failed'
                raise RuntimeError('inherited plan failed official revalidation')
        else:
            if compact and ncores > 1:
                from .bootstrap import first_plan
                began = time.monotonic()
                bootstrap = first_plan(graph, ncores)
                stats['construction_seconds'] += time.monotonic() - began
                evaluate(bootstrap, 'native_bootstrap')
            if best_plan is None:
                evaluate(fallback, 'whole_graph_fallback')
        totals = defaultdict(float)
        tail = {}
        for v in reversed(graph.id_order):
            raw_work = graph.ops[v]['cycles']
            totals[graph.ops[v]['pipe']] += raw_work
            tail[v] = raw_work + max((tail[w] for w in graph.succs[v]), default=0)
        global_bound = max(max(totals.values(), default=0) / ncores,
                           max(tail.values(), default=0),
                           core.route_bytes(graph, fallback) / settings['bandwidth'])
        record['global_lower_bound'] = global_bound
        if ncores == 1:
            record['termination'] = 'fixed_singlecore'
        else:
            from . import structure, pipeline
            from .activation import activation_plans

            def row_for(plan, label):
                result = last_evaluation_result
                if result is not None and result['status'] == 'ok':
                    return objective(result), plan, result, label
                return None

            seed_rows = [row_for(best_plan, record['source'])]

            def legacy_seeds():
                for weight in (0., 1.):
                    yield f'native_lambda_{weight:g}', core.construct(
                        graph, ncores, lambda_weight=weight, settings=settings)

            seed_sources = [iter(legacy_seeds())]
            if ablation != 'construction-only':
                seed_sources = [iter(structure.seed_plans(graph, ncores, settings, deadline=deadline)),
                                iter(pipeline.seed_plans(graph, ncores, settings, deadline=deadline)),
                                *seed_sources]
                if inherited_plan is not None:
                    # Native constructions retain their order and coverage.
                    seed_sources.append(iter(activation_plans(
                        graph, inherited_plan, settings, deadline=deadline, limit=6)))
            seed_cutoff = started + min(time_limit * .25, 180.)
            seed_limit = (8 if inherited_plan is not None else 6) if compact else (24 if inherited_plan is not None else 18)
            seed_count = 0
            while seed_sources and seed_count < seed_limit:
                active = []
                for source in seed_sources:
                    check(reserve=True)
                    began = time.monotonic()
                    try:
                        label, candidate = next(source)
                    except StopIteration:
                        continue
                    stats['construction_seconds'] += time.monotonic() - began
                    active.append(source)
                    seed_count += 1
                    stats['seed_candidates'] += 1
                    evaluate(candidate, label)
                    row = row_for(candidate, label)
                    if row is not None:
                        seed_rows.append(row)
                    if seed_count >= seed_limit or (seed_count >= 4 and time.monotonic() >= seed_cutoff):
                        active = []
                        break
                seed_sources = active
            if warm_start is not None:
                candidate = core.Plan.from_dict(graph, json.loads(Path(warm_start).read_text()))
                if candidate.ncores != ncores:
                    raise ValueError('warm-start core count mismatch')
                evaluate(candidate, 'explicit_warm_start')
                row = row_for(candidate, 'explicit_warm_start')
                if row is not None:
                    seed_rows.append(row)
            seed_rows = [row for row in seed_rows if row is not None]
            if best_plan is None:
                raise RuntimeError('no officially executable seed found')

            native_rows = [row for row in seed_rows if row[3].startswith(('structure_', 'pipe_', 'native_'))]
            # The first representative is exactly the best native construction,
            # independent of how strong the inherited incumbent happens to be.
            representatives = polishing_seeds(graph, seed_rows, limit=3 if compact else 8)
            if not representatives:
                representatives = [min(seed_rows, key=lambda row: row[0])]
            reference_seed = min(native_rows, key=lambda row: row[0]) if native_rows else representatives[0]
            polished, reference_variants = [], []
            polish_cutoff = started + min(time_limit * .45, 360.)
            for index, row in enumerate(representatives):
                if index and time.monotonic() >= polish_cutoff:
                    # Unpolished families remain independent search starts.
                    polished.extend(representatives[index:])
                    break
                variants = [row]
                if ablation not in ('construction-only', 'no-repair'):
                    began = time.monotonic()
                    for label, candidate in pipeline.reorder_plans(
                            graph, row[1], settings, result=row[2], limit=1 if compact else 3):
                        check(reserve=True)
                        stats['seed_reorders'] += 1
                        evaluate(candidate, row[3] + '_' + label)
                        candidate_row = row_for(candidate, row[3] + '_' + label)
                        if candidate_row is not None:
                            variants.append(candidate_row)
                    stats['seed_reorder_seconds'] += time.monotonic() - began
                polished.append(min(variants, key=lambda item: item[0]))
                if row[1].fingerprint() == reference_seed[1].fingerprint():
                    reference_variants = variants

            # One protected route follows the native construction basin. Other
            # assignments receive their own incumbent and bounded frontier.
            reference = Route(graph, 'native_reference', native_rows + reference_variants or [reference_seed], True, compact=compact)
            routes = [reference, Route(graph, 'route_native_joint', [reference.best], compact=compact)]
            route_maps = {assignment_key(graph, reference.best[1])}
            for row in diverse_rows(graph, polished, 8):
                mapping = assignment_key(graph, row[1])
                if mapping in route_maps:
                    continue
                route_maps.add(mapping)
                routes.append(Route(graph, 'route_' + row[3], [row], compact=compact))
                if len(routes) >= (4 if compact else 6):
                    break
            record['routes'] = [route.stats for route in routes]
            record['portfolio_policy'] = 'protected native route; independent local objectives; time-weighted fair service'

            def assess(route, candidate, label):
                evaluate(candidate, label)
                row = row_for(candidate, label)
                if row is None:
                    return 0, None
                improvement = route.offer(row)
                if improvement:
                    stats['local_improvements'] += 1
                return improvement, row[2]

            def joint(route, plan, result, label):
                mapping = assignment_key(graph, plan)
                if (route.reference or ablation == 'no-repair' or mapping in route.reordered
                        or route.stats['fifo_calls'] >= (4 if compact else 16)):
                    return 0, None
                route.reordered.add(mapping)
                check(reserve=True)
                began = time.monotonic()
                variant = next(pipeline.reorder_plans(graph, plan, settings, result=result, limit=1), None)
                stats['coupled_reorder_seconds'] += time.monotonic() - began
                if variant is None:
                    return 0, None
                route.stats['fifo_calls'] += 1
                stats['coupled_reorders'] += 1
                return assess(route, variant[1], label + '_fifo_joint')

            def repair(route, plan, result, label):
                key = plan.fingerprint()
                if ablation == 'no-repair' or not route.repair_allowed(key):
                    stats['repair_skipped'] += 1
                    return 0, None
                route.repaired.add(key)
                route.stats['repair_calls'] += 1
                began = time.monotonic()
                improvement, repaired_result = 0, None
                try:
                    profile = expand(plan, result)
                    if profile.get('status', 'ok') == 'ok':
                        repair_started = time.monotonic()
                        candidate = core.repair_order(graph, plan, profile=profile, max_moves=2, settings=settings)
                        stats['repair_seconds'] += time.monotonic() - repair_started
                        if candidate is not None:
                            improvement, repaired_result = assess(route, candidate, label)
                finally:
                    route.repair_cost += time.monotonic() - began
                    route.stats['repair_seconds'] = route.repair_cost
                    route.repair_failures = 0 if improvement else route.repair_failures + 1
                return improvement, repaired_result

            def explore(route):
                route.stats['status'] = 'searching'
                while True:
                    check(reserve=True)
                    row = route.next_center()
                    if row is None:
                        route.stats['status'] = 'restricted_neighborhood_exhausted'
                        return
                    _, center, center_result, _ = row
                    if len(route.visited) > 1:
                        stats['elite_restarts'] += 1
                    if row[0] > route.best[0]:
                        stats['secondary_centers_expanded'] += 1
                    # Every independent start gets its own ordering opportunity;
                    # a strong solution in another route cannot suppress it.
                    improvement, _ = joint(route, center, center_result, 'center')
                    yield
                    if improvement == 2:
                        continue
                    # Revisit idle cores after accepted remappings. This is
                    # bounded by distinct assignments and the shared deadline.
                    mapping = assignment_key(graph, center)
                    if (not route.reference and len(set(center.core_of.values())) < ncores
                            and mapping not in route.activated and len(route.activated) < 4):
                        route.activated.add(mapping)
                        stats['activation_rounds'] += 1
                        for label, candidate in activation_plans(graph, center, settings, deadline=deadline, limit=3):
                            route.stats['activation_calls'] += 1
                            local, result = assess(route, candidate, label)
                            yield
                            if result is not None and result['makespan'] <= route.best[0][0] * 1.15:
                                extra, _ = joint(route, candidate, result, label)
                                yield
                                local = max(local, extra)
                            if local == 2:
                                break
                        if route.best[0] < row[0]:
                            continue
                    for sweep in range(1 if compact else 2):
                        primary_found = False
                        for family in (1, 2, 0):
                            check(reserve=True)
                            stats['batches'] += 1
                            if family == 0 and global_bound > 0 and route.best[0][0] <= global_bound * 1.01:
                                continue
                            # Profile cache is bounded across routes, not cleared
                            # whenever service switches to another route.
                            while len(profile_cache) > 8:
                                del profile_cache[next(iter(profile_cache))]
                            profile = expand(center, center_result)
                            if (family == 0 and sweep == 0
                                    and (profile.get('lifetime_hotspots') or center_result.get('spill_bytes', 0))):
                                local, _ = repair(route, center, center_result, 'lifetime_repair')
                                yield
                                if local == 2:
                                    primary_found = True
                                    break
                            generation_started = time.monotonic()
                            raw = neighborhoods(graph, center, profile, center_result['official_result'], family,
                                no_group=ablation == 'no-group', no_repair=True if family == 0 else ablation == 'no-repair',
                                settings=settings)
                            stats['generation_seconds'] += time.monotonic() - generation_started
                            began = time.monotonic()
                            ranked, seen = [], set()
                            for candidate in raw:
                                check()
                                stats['generated'] += 1
                                key = candidate.plan.fingerprint()
                                pending_repair = sweep > 0 and candidate.needs_repair and ablation != 'no-repair'
                                # A result evaluated by another route is still
                                # eligible here; only this route's visited work is skipped.
                                if key in seen or (key in route.evaluated and not pending_repair):
                                    stats['duplicates'] += 1
                                    continue
                                seen.add(key)
                                try:
                                    candidate.plan.validate(graph)
                                except ValueError:
                                    continue
                                # Bound against this route's incumbent. Applying
                                # the global guard here would kill independent progress.
                                if pipe_bound(graph, candidate.plan) > route.best[0][0]:
                                    stats['safe_pruned'] += 1
                                    continue
                                ranked.append((cheap(candidate.plan), candidate))
                            stats['screening_seconds'] += time.monotonic() - began
                            ranked.sort(key=lambda item: (item[0], item[1].label))
                            pools = defaultdict(list)
                            for _, candidate in ranked:
                                pools[candidate.kind].append(candidate)
                            ordered = [pool[index] for index in range(max((len(v) for v in pools.values()), default=0))
                                       for pool in pools.values() if index < len(pool)]
                            remaining = deadline - time.monotonic()
                            cost = max(.05, max(durations[-4:], default=.05))
                            quota = min(6 if compact else 12, max(3, int(remaining / (24 * cost))))
                            if sweep:
                                quota = min(18, quota * 2)
                            secondary_center = None
                            for index, candidate in enumerate(ordered[:quota]):
                                check(reserve=True)
                                if index % 3 == 0:
                                    stats['candidate_pages'] += 1
                                improvement, result = assess(route, candidate.plan, candidate.label)
                                yield
                                if improvement == 2:
                                    primary_found = True
                                    break
                                if improvement == 1:
                                    secondary_center = candidate.plan, result
                                if (result is not None and result['status'] == 'ok'
                                        and result['makespan'] <= route.best[0][0] * 1.10
                                        and assignment_key(graph, candidate.plan) != assignment_key(graph, center)):
                                    local, _ = joint(route, candidate.plan, result, candidate.label)
                                    yield
                                    if local == 2:
                                        primary_found = True
                                        break
                                    if local == 1:
                                        secondary_center = route.best[1:3]
                                should_repair = (candidate.needs_repair and ablation != 'no-repair'
                                    and (sweep > 0 or (result is not None and result.get('spill_bytes', 0) > 0)))
                                if should_repair:
                                    local, _ = repair(route, candidate.plan, result, candidate.label + '_repair')
                                    yield
                                    if local == 2:
                                        primary_found = True
                                        break
                                    if local == 1:
                                        secondary_center = route.best[1:3]
                            if primary_found:
                                break
                            if secondary_center is not None:
                                center, center_result = secondary_center
                        if primary_found:
                            break

            finished = 'construction_only' if ablation == 'construction-only' else 'restricted_neighborhood_exhausted'
            active_routes = [] if ablation == 'construction-only' else list(routes)
            iterators = {route.name: explore(route) for route in active_routes}
            while active_routes:
                check(reserve=True)
                if global_bound > 0 and best_key[0] <= global_bound * 1.005:
                    finished = 'primary_bound_gap'
                    break
                current_route = select_route(active_routes)
                began = time.monotonic()
                try:
                    next(iterators[current_route.name])
                except StopIteration:
                    active_routes.remove(current_route)
                finally:
                    current_route.seconds += time.monotonic() - began
                    current_route.stats.update(seconds=current_route.seconds,
                                               steps=current_route.stats['steps'] + 1)
                    stats['route_steps'] += 1
            current_route = None
            record['termination'] = finished
    except (Deadline, TimeoutError):
        record['termination'] = 'time_limit'
    except EvaluationLimit:
        record['termination'] = 'evaluation_limit'
    except Exception as exc:
        record.update(termination='solver_error', error=f'{type(exc).__name__}: {safe_message(exc)}')
        raise
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_alarm)
        record['status'] = ('complete_with_error' if record.get('termination') == 'solver_error' else 'complete') if best_plan is not None else 'failed'
        record['solution_available'] = best_plan is not None
        save_record(force=True)
    # Standalone solve also emits the official two-field plan and trace.
    if export_results:
        from .artifacts import recover
        case = Path(graph_path).stem
        recover(output, case, ncores)
    return record
