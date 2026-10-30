"""Read-only adapter for the supplied scene-B evaluator.

The boundary builder below faithfully exposes the pre-spill portion of the
official ``_build_scene_b_tasks``. Evaluation itself always calls the original
official implementation. No official source or hardware setting is modified.
"""
from __future__ import annotations

from collections import defaultdict
import json
import math
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True
OFFICIAL_DIR = Path(__file__).resolve().parent / "official_code"
if str(OFFICIAL_DIR) not in sys.path:
    sys.path.insert(0, str(OFFICIAL_DIR))

from evaluation_validation import EvaluationValidationError, read_evaluation_config, validate_graph
from multicore_cut_evaluate_problem_1 import _copy_traffic_bytes, _original_tensor_views
from multicore_cut_evaluate_problem_2 import (
    SceneBEvaluationError, _prioritize_task_seq, evaluate_scene_b, read_scene_b_config,
)
from schedule_step1 import step1_schedule
from schedule_step2 import Step2SchedulingError, _build_extended_graph, step2_spill_insertion
from schedule_step3 import Step3SchedulingError
from stub_multicore_cut_and_schedule import MulticoreCutError, derive_multicore_plan

INVALID_ERRORS = (EvaluationValidationError, MulticoreCutError,
                  SceneBEvaluationError, Step2SchedulingError, Step3SchedulingError)


def load_graph(path):
    graph = json.loads(Path(path).read_text(encoding="utf-8"))
    validate_graph(graph)
    return graph


def load_settings(config_path):
    """Only the official parser supplies capacity, bandwidth and release delay."""
    settings = read_evaluation_config(config_path)
    settings["cross_core_copy_delay"] = read_scene_b_config(config_path)[
        "cross_core_copy_delay_cycles"]
    return settings


def _plain_graph(graph):
    return graph.graph if hasattr(graph, "graph") else graph


def _plain_plan(plan):
    return plan.to_dict() if hasattr(plan, "to_dict") else plan


def build_boundary_graphs(graph, plan, bandwidth=1):
    """Return official-equivalent merged Tasks *before* Step1/2/3.

    ``bandwidth=1`` is harmless for byte-only callers: COPY durations never
    enter route_cost. Profile callers pass the actual configured bandwidth.
    ID allocation, COPY ownership and insertion order follow official code.
    """
    graph, plan = _plain_graph(graph), _plain_plan(plan)
    view = derive_multicore_plan(graph, plan)
    mapping, orders = view["mapping"], view["core_orders"]
    ops = {op["id"]: op for op in graph["ops"]}
    tensors = {t["id"]: t for t in graph["tensors"]}
    core_of = {op: view["core_by_subgraph"][block] for op, block in mapping.items()}
    rank = {k: {b: i for i, b in enumerate(order)} for k, order in orders.items()}
    producers, consumers, direct_edges = _original_tensor_views(graph)
    tasks = {k: {"ops": [], "tensors": {}, "edges": [], "op_subgraph": {},
                 "subgraph_order": orders[k]} for k in range(view["num_cores"])}
    for op, block in mapping.items():
        tasks[core_of[op]]["ops"].append(dict(ops[op]))
        tasks[core_of[op]]["op_subgraph"][op] = block
    next_id = max([*ops, *tensors, 0]) + 1
    links, ledger = [], []

    def new_id():
        nonlocal next_id
        value, next_id = next_id, next_id + 1
        return value

    def add_tensor(core, tensor):
        tasks[core]["tensors"].setdefault(tensor["id"], dict(tensor))

    def add_copy(core, local_tid, size, block, kind, ddr_tid=None):
        ddr_tid = new_id() if ddr_tid is None else ddr_tid
        copy_id = new_id()
        add_tensor(core, {"id": ddr_tid, "pos": "DDR", "size": size})
        tasks[core]["ops"].append({
            "id": copy_id, "op": kind,
            "pipe": "PIPE_MTE2" if kind == "COPY_IN" else "PIPE_MTE3",
            "cycles": max(1, math.ceil(size / bandwidth)),
        })
        tasks[core]["op_subgraph"][copy_id] = block
        a, b = (ddr_tid, local_tid) if kind == "COPY_IN" else (local_tid, ddr_tid)
        tasks[core]["edges"].extend([
            {"source": a, "target": copy_id}, {"source": copy_id, "target": b}])
        return copy_id

    for tid in sorted(tensors):
        tensor = tensors[tid]
        prod = sorted(op for op in producers.get(tid, ()) if op in mapping)
        cons = sorted(op for op in consumers.get(tid, ()) if op in mapping)
        source_cores, target_cores = sorted({core_of[op] for op in prod}), sorted({core_of[op] for op in cons})
        local = dict(tensor)
        if local["pos"] == "DDR":
            local["pos"] = "UB"
        for core in sorted(set(source_cores) | set(target_cores)):
            add_tensor(core, local)
            tasks[core]["edges"].extend(
                {"source": op, "target": tid} for op in prod if core_of[op] == core)
            tasks[core]["edges"].extend(
                {"source": tid, "target": op} for op in cons if core_of[op] == core)
        first = {k: min((mapping[op] for op in cons if core_of[op] == k), key=rank[k].get)
                 for k in target_cores}
        last = {k: max((mapping[op] for op in prod if core_of[op] == k), key=rank[k].get)
                for k in source_cores}
        if cons and not prod:
            for core in target_cores:
                cid = add_copy(core, tid, tensor["size"], first[core], "COPY_IN")
                ledger.append({"key": ["input", tid, core], "kind": "input", "tensor_id": tid,
                               "target_core": core, "target_block": first[core],
                               "target_copy_in_id": cid, "bytes": tensor["size"]})
        output = any(ops[op]["op"] == "COPY_OUT" for op in consumers.get(tid, ()) if op in ops)
        if prod and (output or not cons):
            for core in source_cores:
                cid = add_copy(core, tid, tensor["size"], last[core], "COPY_OUT")
                ledger.append({"key": ["output", tid, core], "kind": "output", "tensor_id": tid,
                               "source_core": core, "source_block": last[core],
                               "source_copy_out_id": cid, "bytes": tensor["size"]})
        for source in source_cores:
            for target in target_cores:
                if source == target:
                    continue
                ddr_tid = new_id()
                out_id = add_copy(source, tid, tensor["size"], last[source], "COPY_OUT", ddr_tid)
                in_id = add_copy(target, tid, tensor["size"], first[target], "COPY_IN", ddr_tid)
                link = {"tensor_id": tid, "size": tensor["size"], "source_core": source,
                        "target_core": target, "source_copy_out_id": out_id, "target_copy_in_id": in_id,
                        "source_block": last[source], "target_block": first[target]}
                links.append(link)
                ledger.append({**link, "key": ["cross", tid, source, target], "kind": "cross",
                               "bytes": 2 * tensor["size"]})

    for index, edge in enumerate(direct_edges):
        src, dst = edge["source"], edge["target"]
        if src not in mapping or dst not in mapping:
            continue
        source, target = core_of[src], core_of[dst]
        if source == target:
            tasks[source]["edges"].append(dict(edge))
            continue
        size = max(0, int(edge.get("data_size", 0)))
        local_tid, ddr_tid = new_id(), new_id()
        for core in (source, target):
            add_tensor(core, {"id": local_tid, "pos": "UB", "size": size})
        tasks[source]["edges"].append({"source": src, "target": local_tid})
        tasks[target]["edges"].append({"source": local_tid, "target": dst})
        out_id = add_copy(source, local_tid, size, mapping[src], "COPY_OUT", ddr_tid)
        in_id = add_copy(target, local_tid, size, mapping[dst], "COPY_IN", ddr_tid)
        link = {"tensor_id": local_tid, "size": size, "source_core": source, "target_core": target,
                "source_copy_out_id": out_id, "target_copy_in_id": in_id,
                "source_block": mapping[src], "target_block": mapping[dst]}
        links.append(link)
        ledger.append({**link, "key": ["direct", index, src, dst, source, target],
                       "kind": "direct", "bytes": 2 * size})
    for task in tasks.values():
        task["graph"] = {"ops": task.pop("ops"), "tensors": list(task.pop("tensors").values()),
                         "edges": task.pop("edges")}
    original = _copy_traffic_bytes(graph)
    total = sum(_copy_traffic_bytes(task["graph"]) for task in tasks.values())
    assert total == sum(item["bytes"] for item in ledger)
    return {"tasks": tasks, "cross_links": links, "plan_view": view,
            "route": {"total_ddr_bytes": total, "original_copy_bytes": original,
                      "partition_added_copy_bytes": total - original,
                      "cross_task_traffic": sum(link["size"] for link in links), "ledger": ledger}}


def route_cost(graph, plan):
    """Exact COPY bytes in the official boundary graph, before automatic spill."""
    return build_boundary_graphs(graph, plan)["route"]


def _lifetimes(graph, seq, op_blocks, core, timelines=None):
    """Whole-Task physical lifetimes; all producers and consumers are retained."""
    prod, cons, _ = _original_tensor_views(graph)
    positions = {op: i for i, op in enumerate(seq)}
    timelines = timelines or {}
    records = []
    for tensor in graph["tensors"]:
        tid = tensor["id"]
        if tensor["pos"] not in ("L1", "UB"):
            continue
        producers = sorted(prod.get(tid, ()), key=positions.get)
        consumers = sorted(cons.get(tid, ()), key=positions.get)
        uses = sorted(set(producers) | set(consumers), key=positions.get)
        if not uses:
            continue
        first, last = uses[0], uses[-1]
        record = {"tensor_id": tid, "logical_tid": tensor.get("logical_tid", tid),
                  "size": tensor["size"], "pos": tensor["pos"], "core": core,
                  "producer_ops": producers, "consumer_ops": consumers,
                  "producer_blocks": sorted({op_blocks[op] for op in producers if op in op_blocks}),
                  "consumer_blocks": sorted({op_blocks[op] for op in consumers if op in op_blocks}),
                  "first_use_op": first, "last_use_op": last,
                  "last_use_block": op_blocks.get(last),
                  "first_index": positions[first], "last_index": positions[last],
                  "spill_bytes": 0}
        actual_prod = [timelines[op] for op in producers if op in timelines]
        actual_cons = [timelines[op] for op in consumers if op in timelines]
        if actual_prod or actual_cons:
            # Allocation is at producer issue, release at *all* readers' completion.
            record["allocation_time"] = min((op["start"] for op in actual_prod), default=0)
            record["release_time"] = max((op["end"] for op in actual_cons or actual_prod), default=0)
            record["actual_last_use_op"] = max(actual_cons or actual_prod, key=lambda op: op["end"])["op_id"]
        records.append(record)
    return records


def _build_profile(graph, plan, settings, result=None):
    """Expand merged Tasks with official Step1/2; do not rerun global simulation.

    ``tensors`` describe pre-spill logical lifetimes in the actual input order
    used by Step2, so their pressure can exceed capacity. ``physical_tensors``
    describe separate renamed incarnations after Step2. Actual times are only
    attached to physical lifetimes when a successful official result is given.
    """
    boundary = build_boundary_graphs(graph, plan, settings["bandwidth"])
    if result is not None and "official_result" in result:
        result = result["official_result"]
    timelines = {core["core_id"]: {op["op_id"]: op for op in core["ops"]}
                 for core in (result or {}).get("per_core_timeline", [])}
    profile = {"status": "ok", "tensors": [], "physical_tensors": [], "cores": {}, "spill_records": [],
               "cross_links": [], "route_bytes": boundary["route"]["total_ddr_bytes"],
               "spill_bytes": 0, "total_ddr_bytes": 0}
    for core, task in boundary["tasks"].items():
        raw = task["graph"]
        seq = _prioritize_task_seq(raw, step1_schedule(raw) if raw["ops"] else [],
                                   task["op_subgraph"], task["subgraph_order"])
        spilled = step2_spill_insertion(raw, seq, settings["capacity"]) if seq else {
            "seq_ext": [], "spill_records": [], "new_ops": [], "new_tensors": [], "new_edges": []}
        expanded = _build_extended_graph(raw, spilled)
        blocks = dict(task["op_subgraph"])
        spill_by_tid = defaultdict(int)
        for record in spilled["spill_records"]:
            byte_count = record["size"] * (1 + int(record["spill_out_copies_data"]))
            spill_by_tid[record["logical_tid"]] += byte_count
            profile["spill_records"].append({**record, "core": core, "bytes": byte_count})
            nxt, prev = blocks.get(record["next_use_op"]), blocks.get(record["prev_use_op"])
            blocks[record["spill_in_id"]] = nxt
            if record["spill_out_id"] is not None:
                blocks[record["spill_out_id"]] = prev if prev is not None else nxt
        logical = _lifetimes(raw, seq, task["op_subgraph"], core)
        physical = _lifetimes(expanded, spilled["seq_ext"], blocks, core, timelines.get(core))
        for item in logical:
            item["spill_bytes"] = spill_by_tid[item["tensor_id"]]
        # Allocate before execution and free after execution at the same index.
        # This is a pressure signal of the no-spill reference order, not a claim
        # of illegal execution or of the simulator's actual physical peak.
        events = {pos: defaultdict(int) for pos in settings["capacity"]}
        for item in logical:
            events[item["pos"]][item["first_index"]] += item["size"]
            events[item["pos"]][item["last_index"] + 1] -= item["size"]
        pressure, hot = {}, {}
        for pos, changes in events.items():
            current = peak = 0
            at = 0
            for index in sorted(changes):
                current += changes[index]
                if current > peak:
                    peak, at = current, index
            pressure[pos], hot[pos] = peak, at
        profile["cores"][core] = {"sequence": seq, "expanded_sequence": spilled["seq_ext"],
                                   "subgraph_order": task["subgraph_order"],
                                   "no_spill_pressure_peak": pressure, "pressure_peak_index": hot,
                                   "spill_bytes": sum(spill_by_tid.values()),
                                   "physical_tensor_count": len(physical)}
        profile["tensors"].extend(logical)
        profile["physical_tensors"].extend(physical)
        profile["spill_bytes"] += sum(spill_by_tid.values())
    transfer_times = {(x["source_core"], x["source_copy_out_id"], x["target_core"], x["target_copy_in_id"]): x
                      for x in (result or {}).get("cross_core_transfers", [])}
    for link in boundary["cross_links"]:
        key = (link["source_core"], link["source_copy_out_id"], link["target_core"], link["target_copy_in_id"])
        profile["cross_links"].append({**link, **transfer_times.get(key, {})})
    profile["total_ddr_bytes"] = profile["route_bytes"] + profile["spill_bytes"]
    profile["lifetime_hotspots"] = sorted(
        profile["tensors"],
        key=lambda item: (-item["spill_bytes"],
                          -item["size"] * (item["last_index"] - item["first_index"]),
                          item["core"], item["tensor_id"]))[:16]
    if result is not None:
        expected = result["data_movement_bytes"]["scheduled_copy_bytes"]
        if profile["total_ddr_bytes"] != expected:
            raise AssertionError("profile boundary/spill bytes disagree with official evaluation")
        profile["memory_peak_by_core"] = result["memory_peak_by_core"]
    return profile


def build_profile(graph, plan, settings, result=None):
    """Return a merged-Task profile or a typed, expected expansion failure."""
    try:
        return _build_profile(graph, plan, settings, result)
    except INVALID_ERRORS as exc:
        return {"status": "timeout" if "max_iter exceeded" in str(exc) else "invalid",
                "error": str(exc), "error_type": type(exc).__name__}


def evaluate(graph, plan, settings, profile=False, deadline=None):
    """Run the unchanged official evaluator; programming errors are not hidden.

    The caller owns interruption/process deadlines. ``deadline`` only prevents
    starting a new evaluation after its monotonic deadline; it cannot interrupt
    the official synchronous evaluator midway.
    """
    if deadline is not None and time.monotonic() >= deadline:
        return {"status": "timeout", "makespan": None, "error": "evaluation deadline reached"}
    started = time.monotonic()
    try:
        result = evaluate_scene_b(_plain_graph(graph), _plain_plan(plan), **settings)
    except INVALID_ERRORS as exc:
        return {"status": "timeout" if "max_iter exceeded" in str(exc) else "invalid",
                "makespan": None, "error": str(exc), "error_type": type(exc).__name__,
                "elapsed_seconds": time.monotonic() - started}
    movement = result["data_movement_bytes"]
    wrapped = {"status": "ok", "makespan": result["makespan"],
               "added_copy_bytes": movement["added_copy_bytes"],
               "total_ddr_bytes": movement["scheduled_copy_bytes"],
               "spill_bytes": movement["spill_added_copy_bytes"],
               "route_bytes": movement["scheduled_copy_bytes"] - movement["spill_added_copy_bytes"],
               "peak_l1_by_core": {k: peaks["L1"] for k, peaks in result["memory_peak_by_core"].items()},
               "peak_ub_by_core": {k: peaks["UB"] for k, peaks in result["memory_peak_by_core"].items()},
               "official_result": result, "execution_profile": None}
    if profile:
        wrapped["execution_profile"] = build_profile(graph, plan, settings, result)
    wrapped["elapsed_seconds"] = time.monotonic() - started
    return wrapped
