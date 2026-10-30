"""Development only. The evaluator is supplied externally and is never packaged.

Pass the repository's C++ parity_runner (a port of the Scene B evaluator).
The solver itself never launches a process or reads this script.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def invoke(command, **kwargs):
    start = time.perf_counter()
    result = subprocess.run([str(x) for x in command], capture_output=True,
                            encoding="utf-8", errors="replace", timeout=300, **kwargs)
    return result, time.perf_counter() - start


def check_plan(graph, plan, cores):
    assert set(plan) == {"node_to_subgraph", "core_schedules"}
    expected = {str(o["id"]) for o in graph["ops"] if o["op"] not in {"COPY_IN", "COPY_OUT"}}
    assert set(plan["node_to_subgraph"]) == expected
    assert len(plan["core_schedules"]) == cores
    scheduled = [s for row in plan["core_schedules"] for s in row]
    assert len(scheduled) == len(set(scheduled))
    assert set(scheduled) == set(plan["node_to_subgraph"].values())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--solver", type=Path, required=True)
    p.add_argument("--baseline", type=Path)
    p.add_argument("--evaluator", type=Path, required=True)
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--cores", type=int, nargs="+", default=[2, 3, 4, 5])
    p.add_argument("--cases", nargs="*")
    p.add_argument("--repeat", action="store_true")
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    rows = []
    graphs = sorted(a.data.glob("*.json"))
    if a.cases:
        graphs = [f for f in graphs if f.stem in a.cases]
    assert graphs, "no input graphs"
    for file in graphs:
        raw = json.loads(file.read_text(encoding="utf-8"))
        digest = hashlib.sha256(file.read_bytes()).hexdigest()
        for cores in a.cores:
            for name, exe in [("chain", a.solver), ("singleton_baseline", a.baseline)]:
                if exe is None:
                    continue
                target = a.out / f"{file.stem}_n{cores}_{name}.json"
                result, seconds = invoke([exe.resolve(), file.resolve(), "-n", cores, "-o", target.resolve()])
                row = dict(case=file.stem, cores=cores, strategy=name, solve_seconds=seconds,
                           returncode=result.returncode, input_sha256=digest, valid=False)
                try:
                    assert result.returncode == 0, result.stderr
                    assert seconds < 90, "solver exceeded 90 seconds"
                    assert not result.stdout.strip(), "solver should not print scores"
                    plan = json.loads(target.read_text(encoding="utf-8"))
                    check_plan(raw, plan, cores)
                    if a.repeat:
                        previous = target.read_bytes()
                        repeated, _ = invoke([exe.resolve(), file.resolve(), f"--cores={cores}",
                                              "--output=" + str(target.resolve())])
                        assert repeated.returncode == 0 and previous == target.read_bytes(), "repeat mismatch"
                        row["repeat_identical"] = True
                    request = dict(action="scene_b", graph=raw, plan=plan,
                                   capacity={"L1":1572864,"UB":131072}, bandwidth=60, delay=1000)
                    evaluated, eval_time = invoke([a.evaluator.resolve()], input=json.dumps(request))
                    score = json.loads(evaluated.stdout)
                    assert evaluated.returncode == 0 and "error" not in score, score.get("error", evaluated.stderr)
                    row.update(valid=True, makespan=score["makespan"],
                               data_movement_bytes=score["data_movement_bytes"], evaluation_seconds=eval_time)
                except (AssertionError, ValueError) as error:
                    row["error"] = str(error)
                rows.append(row)
                print(json.dumps({k:v for k,v in row.items() if k not in {"data_movement_bytes","input_sha256"}}, ensure_ascii=False), flush=True)
                (a.out/"report.json").write_text(json.dumps(dict(
                    evaluator="external C++ Scene B port; not a new official evaluator",
                    rows=rows), indent=2), encoding="utf-8")
    assert all(r["valid"] for r in rows), "one or more evaluations failed; inspect report.json"


if __name__ == "__main__":
    main()
