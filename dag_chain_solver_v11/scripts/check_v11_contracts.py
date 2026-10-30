"""Record reproducible CLI/graph-contract checks for the v11 build."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
out = root / 'runs/validation'
out.mkdir(parents=True, exist_ok=True)

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

sources = {p.relative_to(root).as_posix(): sha(p)
           for p in [*sorted((root/'src').glob('*')), root/'CMakeLists.txt'] if p.is_file()}
report = dict(source_sha256=sources, rows=[], completed=False)
for name, build, script in (
    ('msvc_cli', 'build_msvc', 'tests/contracts.py'),
    ('mingw_cli', 'build_mingw', 'tests/contracts.py'),
    ('msvc_graph_26', 'build_msvc', 'tests/rules_contracts.py'),
):
    solver = root/build/'bin/npu_sched.exe'
    command = [sys.executable, str(root/script), '--solver', str(solver),
               '--out', str(out/('contracts_'+name.split('_')[0]))]
    result = subprocess.run(command, capture_output=True, timeout=120)
    stdout = out/(name+'.stdout.txt')
    stderr = out/(name+'.stderr.txt')
    stdout.write_bytes(result.stdout)
    stderr.write_bytes(result.stderr)
    report['rows'].append(dict(name=name, command=command, exit_code=result.returncode,
        stdout_log=stdout.relative_to(root).as_posix(), stdout_sha256=sha(stdout),
        stderr_log=stderr.relative_to(root).as_posix(), stderr_sha256=sha(stderr),
        solver_path=solver.relative_to(root).as_posix(), solver_sha256=sha(solver),
        checker_path=script, checker_sha256=sha(root/script)))
    if result.returncode:
        break
report['completed'] = len(report['rows']) == 3 and all(r['exit_code'] == 0 for r in report['rows'])
(out/'contracts_checks.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(dict(completed=report['completed'], checks=len(report['rows']))))
raise SystemExit(0 if report['completed'] else 1)
