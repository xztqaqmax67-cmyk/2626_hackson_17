"""Check graph-input contracts through the CLI, without importing any evaluator."""
import argparse
import copy
import json
from pathlib import Path
import subprocess


PIPES = ("PIPE_MTE2", "PIPE_MTE3", "PIPE_M", "PIPE_V", "PIPE_FIX")
BASE = {
    "tensors": [{"id": i, "pos": pos, "size": 16}
                for i, pos in [(1, "DDR"), (2, "UB"), (3, "UB"), (4, "DDR")]],
    "ops": [{"id": 10, "op": "COPY_IN", "pipe": "PIPE_MTE2", "cycles": -1},
            {"id": 11, "op": "CUSTOM_KERNEL", "pipe": "PIPE_FIX", "cycles": 4},
            {"id": 12, "op": "COPY_OUT", "pipe": "PIPE_MTE3", "cycles": -1}],
    "edges": [{"source": a, "target": b}
              for a, b in [(1, 10), (10, 2), (2, 11), (11, 3), (3, 12), (12, 4)]],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    solver = args.solver.resolve()
    root = args.out.resolve() / "rules_contracts"
    root.mkdir(parents=True, exist_ok=True)
    count = 0

    def run(name, graph, succeeds=False):
        nonlocal count
        text = graph if isinstance(graph, str) else json.dumps(graph)
        source = root / (name + ".json")
        target = root / (name + "_plan.json")
        source.write_text(text, encoding="utf-8")
        preserved = b"existing output must survive invalid input\n"
        target.write_bytes(preserved)
        result = subprocess.run(
            [str(solver), str(source), "-n", "2", "-o", str(target)],
            capture_output=True, timeout=30, cwd=root,
        )
        expected = 0 if succeeds else 3
        assert result.returncode == expected, (
            name, result.returncode, result.stdout, result.stderr)
        assert not result.stdout.strip(), (name, "generator printed to stdout")
        assert source.read_text(encoding="utf-8") == text, (name, "input changed")
        if succeeds:
            plan = json.loads(target.read_text(encoding="utf-8"))
            eligible = {str(op["id"]) for op in graph["ops"]
                        if op["op"] not in {"COPY_IN", "COPY_OUT"}}
            assert set(plan) == {"node_to_subgraph", "core_schedules"}, name
            assert set(plan["node_to_subgraph"]) == eligible, name
            assert len(plan["core_schedules"]) == 2, name
            scheduled = [sg for order in plan["core_schedules"] for sg in order]
            assert len(scheduled) == len(set(scheduled)), name
            assert set(scheduled) == set(plan["node_to_subgraph"].values()), name
            return_value = plan
        else:
            assert target.read_bytes() == preserved, (name, "invalid input replaced output")
            return_value = None
        count += 1
        return return_value

    def changed(name, change):
        graph = copy.deepcopy(BASE)
        change(graph)
        run(name, graph)

    run("copy_minus_one_and_unknown_compute_name", BASE, succeeds=True)
    five_pipes = {
        "tensors": [],
        "ops": [{"id": i, "op": "CUSTOM_" + str(i), "pipe": pipe, "cycles": i}
                for i, pipe in enumerate(PIPES)],
        "edges": [],
    }
    run("all_five_explicit_pipes_and_zero_cycles", five_pipes, succeeds=True)
    changed("missing_pipe", lambda g: g["ops"][1].pop("pipe"))
    changed("unknown_pipe", lambda g: g["ops"][1].update(pipe="PIPE_UNKNOWN"))
    changed("copy_missing_pipe", lambda g: g["ops"][0].pop("pipe"))
    changed("op_tensor_id_collision", lambda g: g["ops"][1].update(id=1))
    changed("duplicate_op_id", lambda g: g["ops"].append(copy.deepcopy(g["ops"][1])))
    changed("duplicate_tensor_id", lambda g: g["tensors"].append(copy.deepcopy(g["tensors"][0])))
    changed("negative_id", lambda g: g["ops"][1].update(id=-1))
    changed("boolean_id", lambda g: g["ops"][1].update(id=True))
    changed("negative_size", lambda g: g["tensors"][0].update(size=-1))
    changed("negative_compute_cycles", lambda g: g["ops"][1].update(cycles=-1))
    changed("invalid_copy_placeholder", lambda g: g["ops"][0].update(cycles=-2))
    changed("boolean_cycles", lambda g: g["ops"][1].update(cycles=True))
    changed("unknown_endpoint", lambda g: g["edges"][0].update(source=999))
    changed("tensor_to_tensor", lambda g: g["edges"].append({"source": 1, "target": 2}))
    changed("duplicate_tensor_edge", lambda g: g["edges"].append(copy.deepcopy(g["edges"][0])))

    def duplicate_direct(graph):
        graph["ops"].append({"id": 13, "op": "CUSTOM_END", "pipe": "PIPE_V", "cycles": 1})
        graph["edges"].extend([
            {"source": 11, "target": 13, "data_size": 16},
            {"source": 11, "target": 13, "data_size": 32},
        ])
    changed("duplicate_direct_edge_different_size", duplicate_direct)

    serialized = json.dumps(BASE)
    run("duplicate_top_level_key", serialized[:-1] + ', "edges": []}')
    run("duplicate_nested_key", serialized.replace('"cycles": 4', '"cycles": 4, "cycles": 5'))
    run("duplicate_escaped_key", serialized.replace('"cycles": 4', '"cycles": 4, "\\u0063ycles": 5'))
    changed("compute_self_cycle", lambda g: g["edges"].append({"source": 11, "target": 11}))
    changed("copy_self_cycle", lambda g: g["edges"].append({"source": 10, "target": 10}))
    changed("tensor_mediated_cycle", lambda g: g["edges"].append({"source": 3, "target": 11}))
    copy_cycle = copy.deepcopy(BASE)
    copy_cycle["edges"] = [{"source": a, "target": b}
                           for a, b in [(10, 2), (2, 12), (12, 3), (3, 10)]]
    run("isolated_cycle_only_copy_and_tensors", copy_cycle)

    writers = {
        "tensors": [{"id": 10, "pos": "UB", "size": 16}],
        "ops": [{"id": 0, "op": "CUSTOM_A", "pipe": "PIPE_M", "cycles": 10},
                {"id": 1, "op": "CUSTOM_B", "pipe": "PIPE_M", "cycles": 10}],
        "edges": [{"source": 0, "target": 10}, {"source": 1, "target": 10}],
    }
    plan = run("multiple_writers_share_core", writers, succeeds=True)
    owners = {sg: core for core, order in enumerate(plan["core_schedules"]) for sg in order}
    assert owners[plan["node_to_subgraph"]["0"]] == owners[plan["node_to_subgraph"]["1"]]
    print(f"{count} graph contract checks passed; no evaluator invoked.")


if __name__ == "__main__":
    main()
