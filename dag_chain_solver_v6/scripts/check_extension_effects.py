"""Audit changed plans and evenly spaced controls; never tune the solver from scores."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    old_root = root.parent/'dag_chain_solver_v4'
    baseline = json.loads((root/'reports/v4_results_1_to_5_baseline.json').read_text(encoding='utf-8'))
    old = {(r['case'],r['cores']):r for r in baseline['rows'] if r['cores']>1}
    generated = json.loads((root/'runs/final/output_checks_msvc/report.json').read_text(encoding='utf-8'))
    assert len(generated['rows'])==40
    executable = root/'build_msvc/bin/chain_diagnostics.exe'
    folder = root/'runs/final/extension_audit'
    folder.mkdir(parents=True,exist_ok=True)
    changed = []
    for row in generated['rows']:
        key = row['case'],row['cores']
        original = old_root/old[key]['plan']
        if sha(original)!=row['plan_sha256']:
            changed.append(key)
    keys = sorted(old)
    selected = sorted(set(changed)|{keys[i] for i in (0,13,26,39)})
    report = dict(version='0.5.0',diagnostic_executable_sha256=sha(executable),
                  changed_plans=len(changed),selection='All changed plans and four evenly spaced controls.',rows=[])
    for case,cores in selected:
        graph = root/'references/hackathon_20261030/data'/f'{case}.json'
        row = dict(case=case,cores=cores,plan_changed=(case,cores) in changed)
        for extensions,name in ((0,'v4_path'),(3,'v5')):
            plan = folder/f'{case}_n{cores}_{name}.json'
            command = [str(executable),str(graph),str(cores),str(plan),'--extensions',str(extensions)]
            process = subprocess.run(command,capture_output=True,text=True,check=True,timeout=90)
            value = json.loads(process.stdout)
            reference = (old_root/old[case,cores]['plan'] if extensions==0 else
                         root/'runs/final/output_checks_msvc'/f'{case}_n{cores}_v5.json')
            # The diagnostic writer uses Windows text mode (CRLF); the formal
            # submission writer uses LF. Only normalize those line endings.
            normalized = plan.read_bytes().replace(b'\r\n',b'\n')
            assert normalized==reference.read_bytes().replace(b'\r\n',b'\n'),(case,cores,name,'plan differs')
            row[name] = dict(**value,plan_sha256=sha(plan),matches_recorded_plan=True,
                             newline_normalized_sha256=hashlib.sha256(normalized).hexdigest())
        assert row['v5']['estimated_cycles']<=row['v4_path']['estimated_cycles']+1e-7
        report['rows'].append(row)
        (root/'reports/extension_audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(f"{case} n{cores}: {row['v5']['label']}; v4 path and v5 match after newline normalization",flush=True)
    report['completed']=True
    (root/'reports/extension_audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
