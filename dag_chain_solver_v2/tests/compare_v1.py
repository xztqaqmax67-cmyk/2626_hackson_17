"""Paired generation timings and external evaluation against immutable v1 results.

The v1 plan is regenerated and must match its previously evaluated bytes before
the archived score is reused. Every v2 plan is freshly evaluated and repeated.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

from verify import check_plan


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def invoke(command, timeout=90, **kwargs):
    start = time.perf_counter()
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            encoding="utf-8", errors="replace", timeout=timeout, **kwargs)
    return result, time.perf_counter() - start


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--v1", type=Path, required=True)
    p.add_argument("--v1-runs", type=Path, required=True)
    p.add_argument("--v2", type=Path, required=True)
    p.add_argument("--evaluator", type=Path, required=True)
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--singlecore-report", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    previous = json.loads((a.v1_runs / "report.json").read_text(encoding="utf-8"))
    indexed = {(x["case"], x["cores"]): x for x in previous["rows"] if x["strategy"] == "chain"}
    baselines = json.loads(a.singlecore_report.read_text(encoding="utf-8-sig"))["baselines"]
    report = dict(evaluator="unchanged external C++ Scene B port; local scores",
                  hardware=dict(L1=1572864, UB=131072, bandwidth=60, delay=1000),
                  baseline="local Scene A single-subgraph single-core evaluator",
                  v1_score_reuse="regenerate v1 plan; require identical bytes and input hash",
                  executables={name: dict(path=str(path.resolve()), sha256=digest(path))
                               for name, path in [("v1", a.v1), ("v2", a.v2), ("evaluator", a.evaluator)]},
                  rows=[])
    for graph_path in sorted(a.data.glob("*.json")):
        graph = json.loads(graph_path.read_text(encoding="utf-8"))
        for cores in [2, 3, 4, 5]:
            old = indexed[graph_path.stem, cores]
            row = dict(case=graph_path.stem, cores=cores, valid=False,
                       input_sha256=digest(graph_path), singlecore_makespan=baselines[graph_path.stem]["makespan"],
                       v1_makespan=old["makespan"], v1_solve_seconds_archived=old["solve_seconds"],
                       v1_data_movement_bytes=old["data_movement_bytes"])
            try:
                assert row["input_sha256"] == old["input_sha256"], "input differs from original run"
                old_plan = a.out / f"{graph_path.stem}_n{cores}_v1.json"
                run, seconds = invoke([a.v1.resolve(), graph_path.resolve(), "-n", cores, "-o", old_plan.resolve()])
                row["v1_solve_seconds"] = seconds
                assert run.returncode == 0, run.stderr
                archived = a.v1_runs / f"{graph_path.stem}_n{cores}_chain.json"
                assert old_plan.read_bytes() == archived.read_bytes(), "v1 output changed; cached score cannot be used"
                row["v1_identical_to_archived"] = True
                new_plan = a.out / f"{graph_path.stem}_n{cores}_v2.json"
                run, seconds = invoke([a.v2.resolve(), graph_path.resolve(), "-n", cores, "-o", new_plan.resolve()])
                row["solve_seconds"] = seconds
                assert run.returncode == 0, run.stderr
                assert seconds < 90 and not run.stdout.strip(), "runtime/output contract"
                plan_bytes = new_plan.read_bytes()
                plan = json.loads(plan_bytes)
                check_plan(graph, plan, cores)
                run, _ = invoke([a.v2.resolve(), graph_path.resolve(), f"--cores={cores}", "--output=" + str(new_plan.resolve())])
                assert run.returncode == 0 and new_plan.read_bytes() == plan_bytes, "v2 repeat mismatch"
                row["repeat_identical"] = True
                row["ops_per_core"] = [len(x) for x in plan["core_schedules"]]
                row["active_cores"] = sum(bool(x) for x in plan["core_schedules"])
                request = dict(action="scene_b", graph=graph, plan=plan,
                               capacity={"L1":1572864, "UB":131072}, bandwidth=60, delay=1000)
                evaluated, seconds = invoke([a.evaluator.resolve()], timeout=300, input=json.dumps(request))
                result = json.loads(evaluated.stdout)
                row["evaluation_seconds"] = seconds
                assert evaluated.returncode == 0 and "error" not in result, result.get("error", evaluated.stderr)
                row.update(valid=True, makespan=result["makespan"], data_movement_bytes=result["data_movement_bytes"],
                           plan_sha256=hashlib.sha256(plan_bytes).hexdigest(),
                           speedup=row["singlecore_makespan"] / result["makespan"],
                           v1_speedup=row["singlecore_makespan"] / old["makespan"],
                           makespan_reduction=1-result["makespan"]/old["makespan"])
            except (AssertionError, ValueError, subprocess.TimeoutExpired) as error:
                row["error"] = str(error)
            report["rows"].append(row)
            (a.out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(json.dumps({key: value for key, value in row.items() if key in
                              {"case", "cores", "valid", "v1_makespan", "makespan", "makespan_reduction",
                               "solve_seconds", "active_cores", "error"}}, ensure_ascii=False), flush=True)
    assert all(x["valid"] for x in report["rows"]), "one or more v2 evaluations failed"


if __name__ == "__main__":
    main()
