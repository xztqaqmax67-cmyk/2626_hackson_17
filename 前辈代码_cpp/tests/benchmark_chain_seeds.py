"""比较纯初解候选；外部评分最佳是研究参考，不是运行期估价的成绩。

生成器接口：parity_runner action=chain_seeds / legacy_seeds。
评估接口：独立调用 action=scene_b；不执行局部迁移或重排序搜索。
同图、硬件、评估器与方案的缓存可以复用，所有候选和失败均保留。
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
HARDWARE = dict(capacity={"L1": 1572864, "UB": 131072}, bandwidth=60, delay=1000)
TRAFFIC = ("original_graph_copy_bytes", "scheduled_copy_bytes", "added_copy_bytes",
           "partition_added_copy_bytes", "spill_added_copy_bytes")


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def ordered(value):
    # 评估器生成 COPY ID 时会遍历映射；缓存与确定性检查必须保留键顺序。
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def digest_bytes(value):
    return hashlib.sha256(value).hexdigest()


def digest_file(path):
    return digest_bytes(path.read_bytes())


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def invoke(executable, request, timeout):
    started = time.perf_counter()
    try:
        process = subprocess.run([str(executable)], input=json.dumps(request),
                                 capture_output=True, encoding="utf-8", errors="replace",
                                 timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, time.perf_counter() - started, f"timeout after {timeout:g} seconds"
    elapsed = time.perf_counter() - started
    try:
        result = json.loads(process.stdout)
    except ValueError:
        return None, elapsed, f"invalid evaluator JSON, exit={process.returncode}: " + process.stderr[-1000:]
    if process.returncode != 0 or (isinstance(result, dict) and "error" in result):
        return None, elapsed, str(result.get("error", process.stderr[-1000:]))
    return result, elapsed, None


def check_plan(graph, plan, cores):
    if set(plan) != {"node_to_subgraph", "core_schedules"}:
        raise ValueError("plan must contain exactly the two submission fields")
    expected = {str(op["id"]) for op in graph["ops"] if op["op"] not in {"COPY_IN", "COPY_OUT"}}
    if set(plan["node_to_subgraph"]) != expected:
        raise ValueError("plan does not cover exactly the non-COPY operators")
    if len(plan["core_schedules"]) != cores:
        raise ValueError("wrong core count")
    scheduled = [block for row in plan["core_schedules"] for block in row]
    if len(scheduled) != len(set(scheduled)) or set(scheduled) != set(plan["node_to_subgraph"].values()):
        raise ValueError("subgraph schedule is missing or duplicated")


def export_csv(path, rows):
    fields = []
    for row in rows:
        for key in row:
            if key not in fields:
                fields.append(key)
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def reference_rows(path):
    if path is None or not path.exists():
        return {}
    result = {}
    for row in json.loads(path.read_text(encoding="utf-8-sig"))["rows"]:
        if row.get("valid"):
            result[(row["case"], row["cores"], row["strategy"])] = row
    return result


def summarize(report, references):
    comparisons = []
    grouped = {}
    for row in report["candidates"]:
        grouped.setdefault((row["case"], row["cores"], row["family"]), []).append(row)
    for group in report["groups"]:
        key = group["case"], group["cores"], group["family"]
        rows = grouped.get(key, [])
        valid = [row for row in rows if row.get("valid")]
        summary = dict(case=key[0], cores=key[1], family=key[2],
                       candidates=len(rows), valid_candidates=len(valid),
                       generation_seconds=group.get("generation_seconds"),
                       construction_seconds=group.get("construction_seconds"),
                       evaluation_seconds=sum(row.get("evaluation_seconds", 0) for row in rows),
                       evaluation_seconds_this_run=sum(row.get("evaluation_seconds_this_run", 0) for row in rows),
                       repeat_identical=group.get("repeat_identical"),
                       generation_error=group.get("error"))
        if rows:
            first = min(rows, key=lambda row: row["index"])
            summary.update(generation_first_label=first["label"],
                           generation_first_plan=first["plan_file"],
                           generation_first_valid=first["valid"],
                           generation_first_error=first.get("error"))
            if first["valid"]:
                summary["generation_first_makespan"] = first["makespan"]
                summary["generation_first_scheduled_copy_bytes"] = first["data_movement_bytes"]["scheduled_copy_bytes"]
            # 保留生成顺序平局；不按外部成绩回退选解。
            proxy = min(rows, key=lambda row: (row["proxy"]["cost"], row["proxy"].get("route_bytes", 0), row["index"]))
            summary.update(proxy_label=proxy["label"], proxy_plan=proxy["plan_file"],
                           proxy_cost=proxy["proxy"]["cost"], proxy_valid=proxy["valid"],
                           proxy_error=proxy.get("error"))
            if proxy["valid"]:
                summary["proxy_makespan"] = proxy["makespan"]
                summary["proxy_scheduled_copy_bytes"] = proxy["data_movement_bytes"]["scheduled_copy_bytes"]
        if valid:
            best = min(valid, key=lambda row: (row["makespan"], row["data_movement_bytes"]["added_copy_bytes"], row["index"]))
            summary.update(oracle_label=best["label"], oracle_plan=best["plan_file"], oracle_makespan=best["makespan"])
            for field in TRAFFIC:
                summary["oracle_" + field] = best["data_movement_bytes"][field]
            if summary.get("proxy_valid") and best["makespan"] > 0:
                summary["proxy_regret_pct"] = 100 * (summary["proxy_makespan"] / best["makespan"] - 1)
            if summary.get("generation_first_valid") and best["makespan"] > 0:
                summary["generation_first_regret_pct"] = 100 * (summary["generation_first_makespan"] / best["makespan"] - 1)
            for ref_strategy, prefix in (("chain", "previous_dag_search"), ("singleton_baseline", "previous_singleton")):
                ref = references.get((key[0], key[1], ref_strategy))
                if ref and ref.get("input_sha256") == group["input_sha256"]:
                    summary[prefix + "_makespan"] = ref["makespan"]
                    summary[prefix + "_scheduled_copy_bytes"] = ref["data_movement_bytes"]["scheduled_copy_bytes"]
                    summary["oracle_reduction_vs_" + prefix + "_pct"] = 100 * (1 - best["makespan"] / ref["makespan"])
        comparisons.append(summary)
    report["comparisons"] = comparisons
    totals = []
    for family in report["families"]:
        rows = [row for row in comparisons if row["family"] == family]
        valid = [row for row in rows if "oracle_makespan" in row]
        total = dict(family=family, groups=len(rows), groups_with_valid_candidate=len(valid),
                     groups_proxy_valid=sum(bool(row.get("proxy_valid")) for row in rows),
                     generation_seconds=sum(row.get("generation_seconds", 0) for row in rows),
                     evaluation_seconds_this_run=sum(row.get("evaluation_seconds_this_run", 0) for row in rows))
        for field in TRAFFIC:
            total["oracle_total_" + field] = sum(row["oracle_" + field] for row in valid)
        for prefix in ("previous_dag_search", "previous_singleton"):
            matched = [row for row in valid if prefix + "_makespan" in row]
            if matched:
                total["matched_" + prefix] = len(matched)
                total["oracle_mean_reduction_vs_" + prefix + "_pct"] = sum(row["oracle_reduction_vs_" + prefix + "_pct"] for row in matched) / len(matched)
                total["oracle_wins_vs_" + prefix] = sum(row["oracle_makespan"] < row[prefix + "_makespan"] for row in matched)
                total["oracle_ties_vs_" + prefix] = sum(row["oracle_makespan"] == row[prefix + "_makespan"] for row in matched)
                total["oracle_losses_vs_" + prefix] = sum(row["oracle_makespan"] > row[prefix + "_makespan"] for row in matched)
        totals.append(total)
    report["summary"] = totals


def save(report, output, references):
    report["semantics"].update(
        proxy="minimum CPP generic proxy cost, then traffic, then generation order; not the chain module's internal prescreen; no external-score fallback",
        generation_first="first emitted candidate; for chain, this is the chain module's internal prescreen choice; for legacy, it is source enumeration order")
    summarize(report, references)
    write_json(output / "report.json", report)
    export_csv(output / "comparison.csv", report["comparisons"])
    flat = []
    for row in report["candidates"]:
        flattened = {k: v for k, v in row.items() if k not in {"proxy", "data_movement_bytes"}}
        flattened.update({"proxy_" + k: v for k, v in row["proxy"].items()})
        flattened.update(row.get("data_movement_bytes", {}))
        flat.append(flattened)
    export_csv(output / "candidates.csv", flat)
    for row in report["comparisons"]:
        for criterion in ("oracle", "proxy", "generation_first"):
            source = row.get(criterion + "_plan")
            if source:
                name = f"{row['case']}_n{row['cores']}_{row['family']}_{criterion}.json"
                plan = json.loads((output / source).read_text(encoding="utf-8"))
                write_json(output / "best" / name, plan)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--evaluator", type=Path, help="默认为 runner；可固定评估器以复用跨版本缓存")
    parser.add_argument("--data", type=Path, default=ROOT / "hackathon_data")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--families", nargs="+", choices=("chain", "legacy"), default=["chain"])
    parser.add_argument("--cores", type=int, nargs="+", default=[4])
    parser.add_argument("--cases", nargs="*")
    parser.add_argument("--limit", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    parser.add_argument("--cache-dir", type=Path, help="跨实验共享精评缓存，默认 out/evaluation_cache")
    parser.add_argument("--reference-report", type=Path,
                        default=ROOT / "tmp/review_dag_chain_a697197/runs/four_core/report.json")
    args = parser.parse_args()
    if args.limit < 1 or args.timeout <= 0 or any(core not in range(2, 6) for core in args.cores):
        parser.error("limit/timeout must be positive and cores must be 2..5")
    args.runner = args.runner.resolve()
    args.evaluator = (args.evaluator or args.runner).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    cache_directory = args.cache_dir or args.out / "evaluation_cache"
    runner_hash, evaluator_hash = digest_file(args.runner), digest_file(args.evaluator)
    graphs = sorted(args.data.glob("*.json"))
    if args.cases:
        available = {path.stem for path in graphs}
        if set(args.cases) - available:
            parser.error("unknown cases: " + ", ".join(sorted(set(args.cases) - available)))
        graphs = [path for path in graphs if path.stem in args.cases]
    if not graphs:
        parser.error("no input graphs")
    references = reference_rows(args.reference_report)
    report = dict(schema=1, benchmark="pure_initial_seeds", hardware=HARDWARE,
                  families=args.families, cores=args.cores, limit=args.limit,
                  generator=str(args.runner), generator_sha256=runner_hash,
                  evaluator=str(args.evaluator), evaluator_sha256=evaluator_hash,
                  evaluation_cache=str(cache_directory.resolve()),
                  semantics={"oracle": "best legal initial candidate selected using external Scene B evaluation; research reference only",
                             "proxy": "minimum CPP generic proxy cost, then traffic, then generation order; not chain internal prescreen; no external-score fallback",
                             "generation_first": "first emitted candidate; chain module internal prescreen choice, or legacy source enumeration order",
                             "search": "no migration or post-construction reorder search is run",
                             "reference": "previous DAG solver includes chain migration; singleton reference is a reduced baseline; neither is equal-budget CPP search",
                             "time": "construction/selection and external evaluation are reported separately; cached evaluations retain original duration"},
                  groups=[], candidates=[])
    started = time.perf_counter()
    for graph_path in graphs:
        graph_bytes = graph_path.read_bytes()
        graph = json.loads(graph_bytes.decode("utf-8-sig"))
        graph_hash = digest_bytes(graph_bytes)
        for cores in args.cores:
            for family in args.families:
                if digest_file(args.runner) != runner_hash or digest_file(args.evaluator) != evaluator_hash:
                    raise RuntimeError("a benchmark executable changed during the run; use a versioned binary snapshot")
                request = dict(action=family + "_seeds", graph=graph, cores=cores,
                               limit=args.limit, **HARDWARE)
                generated, elapsed, error = invoke(args.runner, request, args.timeout)
                group = dict(case=graph_path.stem, cores=cores, family=family,
                             input_sha256=graph_hash, generation_seconds=elapsed)
                report["groups"].append(group)
                if error:
                    group["error"] = error
                    save(report, args.out, references)
                    print(json.dumps(group, ensure_ascii=False), flush=True)
                    continue
                group["construction_seconds"] = generated.get("construction_seconds")
                seeds = generated["seeds"]
                if args.repeat:
                    again, repeat_time, repeat_error = invoke(args.runner, request, args.timeout)
                    group["repeat_seconds"] = repeat_time
                    group["repeat_identical"] = not repeat_error and ordered(seeds) == ordered(again["seeds"])
                    if not group["repeat_identical"]:
                        group["error"] = repeat_error or "candidate generation is not deterministic"
                for index, seed in enumerate(seeds):
                    label, plan, proxy = seed["label"], seed["plan"], seed["proxy"]
                    if not math.isfinite(proxy["cost"]):
                        raise ValueError(f"nonfinite proxy cost: {label}")
                    slug = re.sub(r"[^a-zA-Z0-9_.-]", "_", label)[:120]
                    plan_file = Path("plans") / f"{graph_path.stem}_n{cores}_{family}_{index:02d}_{slug}.json"
                    write_json(args.out / plan_file, plan)
                    plan_hash = digest_bytes(ordered(plan).encode("utf-8"))
                    cache_key = digest_bytes(canonical(dict(input=graph_hash, plan=plan_hash, cores=cores,
                                                           hardware=HARDWARE, evaluator=evaluator_hash)).encode("utf-8"))
                    cache_file = cache_directory / (cache_key + ".json")
                    row = dict(case=graph_path.stem, cores=cores, family=family, index=index,
                               label=label, plan_file=plan_file.as_posix(), plan_sha256=plan_hash,
                               input_sha256=graph_hash, proxy=proxy, valid=False, evaluation_seconds_this_run=0)
                    try:
                        check_plan(graph, plan, cores)
                        if cache_file.exists() and not args.no_cache:
                            measured = json.loads(cache_file.read_text(encoding="utf-8"))
                            row["evaluation_cached"] = True
                        else:
                            evaluated, seconds, eval_error = invoke(args.evaluator, dict(
                                action="scene_b", graph=graph, plan=plan, **HARDWARE), args.timeout)
                            measured = dict(valid=False, evaluation_seconds=seconds)
                            row["evaluation_seconds_this_run"] = seconds
                            row["evaluation_cached"] = False
                            if eval_error:
                                measured["error"] = eval_error
                            else:
                                measured.update(valid=True, makespan=evaluated["makespan"],
                                                data_movement_bytes=evaluated["data_movement_bytes"])
                            write_json(cache_file, measured)
                        row.update(measured)
                    except (KeyError, ValueError, TypeError) as failure:
                        row["error"] = str(failure)
                    report["candidates"].append(row)
                    report["wall_seconds"] = time.perf_counter() - started
                    save(report, args.out, references)
                    print(json.dumps({k: row[k] for k in ("case", "cores", "family", "index", "label", "valid", "makespan", "error", "evaluation_seconds_this_run") if k in row}, ensure_ascii=False), flush=True)
                save(report, args.out, references)
                print(json.dumps(report["comparisons"][-1], ensure_ascii=False), flush=True)
    report["wall_seconds"] = time.perf_counter() - started
    report["inputs_unchanged"] = all(digest_file(path) == next(g["input_sha256"] for g in report["groups"] if g["case"] == path.stem) for path in graphs)
    save(report, args.out, references)
    print(json.dumps(report["summary"], ensure_ascii=False, indent=2))
    if not report["inputs_unchanged"] or any(g.get("error") for g in report["groups"]) or any(row["valid_candidates"] == 0 for row in report["comparisons"]):
        raise SystemExit("one or more groups failed; inspect report.json")


if __name__ == "__main__":
    main()
