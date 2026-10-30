"""Compare GCC plans with the validated MSVC plans on representative workloads."""
import argparse
import json
from pathlib import Path
import sys
from check_outputs import generate, sha256
from check_rules_outputs import check_plan


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--solver', type=Path, default=root/'build_mingw/bin/npu_sched.exe')
    a = p.parse_args()
    out = root/'runs/final/compiler_comparison'
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for case in ['case_00', 'case_03', 'case_05', 'case_08']:
        graph = root/'references/hackathon_20261030/data'/f'{case}.json'
        plan = out/f'{case}_n4_gcc.json'
        timing = generate(a.solver, graph, 4, plan)
        check_plan(json.loads(graph.read_text(encoding='utf-8')), json.loads(plan.read_text(encoding='utf-8')), 4)
        reference = root/'runs/final/output_checks_msvc'/f'{case}_n4_v5.json'
        assert plan.read_bytes() == reference.read_bytes(), (case, 'compiler output differs')
        rows.append(dict(case=case, cores=4, byte_identical=True, generation=timing,
                         plan_sha256=sha256(plan)))
        print(case, 'MSVC/GCC byte-identical', flush=True)
    report = dict(solver_sha256=sha256(a.solver), rows=rows, linux_tested=False)
    (root/'reports/compiler_comparison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()

