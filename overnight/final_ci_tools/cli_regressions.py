"""Regression checks for the 0.8.3 CLI; keep all fixtures outside the submission."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--solver", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    solver = args.solver.resolve()
    parent = args.out.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="cli_083_", dir=parent))
    # These artifacts remain in the requested output directory for review.
    graph = {
        "tensors": [{"id": i, "pos": pos, "size": 16}
                    for i, pos in [(1, "DDR"), (2, "UB"), (3, "UB"), (4, "DDR")]],
        "ops": [{"id": 10, "op": "COPY_IN", "pipe": "PIPE_MTE2", "cycles": 1},
                {"id": 11, "op": "ADD", "pipe": "PIPE_V", "cycles": 4},
                {"id": 12, "op": "COPY_OUT", "pipe": "PIPE_MTE3", "cycles": 1}],
        "edges": [{"source": a, "target": b}
                  for a, b in [(1, 10), (10, 2), (2, 11), (11, 3), (3, 12), (12, 4)]],
    }
    target = root / "中文 plan.json"
    source = Path(str(target) + ".tmp")
    original = json.dumps(graph).encode("utf-8")
    source.write_bytes(original)
    collision = Path(str(target) + ".tmp.0")
    collision.write_bytes(b"existing unrelated file\n")
    directory = Path(str(target) + ".tmp.1")
    directory.mkdir()
    marker = directory / "plan.json"
    marker.write_bytes(b"existing unrelated directory content\n")
    rows = []

    def run(name, command, expected):
        process = subprocess.run([str(solver), *map(str, command)], cwd=root,
                                 stdin=subprocess.DEVNULL, capture_output=True, timeout=90)
        assert process.returncode == expected, (name, process.returncode, process.stderr)
        if name != "version":
            assert not process.stdout, (name, process.stdout)
        rows.append({"test": name, "exit_code": process.returncode})
        return process

    assert b"0.8.3" in run("version", ["--version"], 0).stdout
    command = [source, "--cores=2", "--output=" + str(target)]
    run("input_matches_old_temporary_path", command, 0)
    first = target.read_bytes()
    run("repeat_atomic_replacement", command, 0)
    assert target.read_bytes() == first
    assert source.read_bytes() == original
    assert collision.read_bytes() == b"existing unrelated file\n"
    assert marker.read_bytes() == b"existing unrelated directory content\n"
    assert not Path(str(target) + ".tmp.2").exists(), "temporary reservation was not cleaned"
    result = json.loads(first)
    assert set(result) == {"node_to_subgraph", "core_schedules"}
    assert result["node_to_subgraph"] == {"11": 11}
    assert len(result["core_schedules"]) == 2

    run("protect_input", [source, "-o", source], 2)
    run("missing_option_value", [source, "-n"], 2)
    run("invalid_core", [source, "-n", "1"], 2)
    run("missing_input", [root / "absent.json"], 3)
    run("output_directory_is_file", [source, "-o", collision / "plan.json"], 4)
    run("output_is_directory", [source, "-o", directory], 4)
    assert marker.read_bytes() == b"existing unrelated directory content\n"
    assert not Path(str(directory) + ".tmp.0").exists(), "failed output left temporary files"
    infeasible = json.loads(original)
    infeasible["tensors"][1]["size"] = 131073
    oversized = root / "oversized.json"
    oversized.write_text(json.dumps(infeasible), encoding="utf-8")
    run("no_feasible_candidate", [oversized, "-n", "2", "-o", target], 5)
    assert target.read_bytes() == first, "failed solving replaced prior output"
    assert source.read_bytes() == original
    (root / "results.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
    print(f"{len(rows)} CLI regressions passed; evidence: {root}")


if __name__ == "__main__":
    main()
