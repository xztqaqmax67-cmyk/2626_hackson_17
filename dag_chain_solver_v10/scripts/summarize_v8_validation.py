"""Combine completed upgrade validations; fail rather than infer missing checks."""
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile


def read(path):
    return json.loads(path.read_text(encoding='utf-8'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[1]
    def evidence(name):
        return dict(path=name, sha256=sha(root / name))
    summary = read(root / 'runs/official_upgrade/summary.json')
    comparison = read(root / 'reports/RESULTS_V8_UPGRADE.json')
    assert summary['completed'] and comparison['completed'] and len(summary['rows']) == 40
    assert comparison['source_sha256'] == summary['source_sha256']
    assert comparison['measured_summary_sha256'] == sha(root / 'runs/official_upgrade/summary.json')
    for name, digest in summary['source_sha256'].items():
        assert sha(root / name) == digest, ('source changed after benchmark', name)
    solver = Path(summary['solver_path'])
    assert sha(solver) == summary['solver_executable_sha256']
    expected_tests = set(re.findall(r'add_test\(NAME\s+(\w+)', (root / 'CMakeLists.txt').read_text()))
    cpp = {}
    for compiler in ('msvc', 'mingw'):
        name = f'build_{compiler}/Testing/Temporary/LastTest.log'
        path = root / name
        log = path.read_text(encoding='utf-8', errors='replace')
        tests = re.findall(r'^\d+/\d+ Testing: (\w+)$', log, re.MULTILINE)
        assert len(tests) == len(expected_tests) and set(tests) == expected_tests, (compiler, tests)
        assert log.count('Test Passed.') == len(expected_tests) and 'Test Failed.' not in log
        binaries = [Path(s) for s in re.findall(r'^Command: "([^"]+)"', log, re.MULTILINE)]
        assert len(binaries) == len(expected_tests)
        assert all(path.stat().st_mtime >= p.stat().st_mtime for p in binaries), 'CTest log predates binary'
        cpp[compiler] = dict(passed=len(tests), tests=tests, log=evidence(name),
                             executable_sha256={p.relative_to(root).as_posix(): sha(p) for p in binaries})
    generic = read(root / 'runs/validation_upgrade/summary.json')
    assert {x['name'] for x in generic} >= {'cli', 'rules', 'synthetic'}
    assert all(x['exit_code'] == 0 for x in generic)
    cli_text = (root / 'runs/validation_upgrade/cli.txt').read_text(encoding='utf-8', errors='replace')
    rules_text = (root / 'runs/validation_upgrade/rules.txt').read_text(encoding='utf-8', errors='replace')
    assert 'CLI contract checks passed' in cli_text
    match = re.search(r'(\d+) graph contract checks passed', rules_text)
    assert match
    synthetic = read(root / 'runs/synthetic_upgrade/report.json')
    assert synthetic['completed'] and synthetic['passed'] == 64 and synthetic['failed'] == 0
    assert synthetic['solver_executable_sha256'] == sha(solver)
    assert all(r['status'] == 'ok' and r['repeat_plan_identical'] for r in synthetic['rows'])
    parity = read(root / 'runs/spill_differential_upgrade/report.json')
    assert parity['completed'] and parity['passed'] == 120 and parity['failed'] == 0
    assert parity['metadata']['solver_sha256'] == sha(solver)
    assert parity['metadata']['diagnostics_sha256'] == sha(root / 'build_msvc/bin/spill_diagnostics.exe')
    for name, digest in parity['metadata']['source_sha256'].items():
        assert sha(root / name) == digest, name
    assert Counter(r['group'] for r in parity['rows']) == dict(public=40, synthetic=64, adversarial=16)
    assert all(r['status'] == 'ok' for r in parity['rows'])
    final = {(r['case'], r['cores']): r for r in summary['rows']}
    for row in parity['rows']:
        if row['group'] == 'public':
            measured = final[row['case'], row['cores']]
            assert row['plan_sha256'] == measured['plan_sha256']
            assert row['input_sha256'] == measured['input_sha256']
            assert row['spill_bytes'] == measured['data_movement_bytes']['spill_added_copy_bytes']
            assert row['route_bytes'] + row['spill_bytes'] == measured['data_movement_bytes']['scheduled_copy_bytes']
    backup_name = 'backups/v8_0_8_0_before_feedback_upgrade_20261031'
    backup = root / backup_name
    manifest = read(backup / 'manifest.json')['files']
    with zipfile.ZipFile(backup / 'snapshot.zip') as archive:
        assert set(archive.namelist()) == set(manifest)
        for name, digest in manifest.items():
            assert hashlib.sha256(archive.read(name)).hexdigest() == digest, name
    # Old source/report evidence must remain reachable even after rebuilding v8.
    for name in ('reports/RESULTS_V8.json', 'reports/RESULTS_V8_zh.md', 'reports/VALIDATION_V8.json'):
        assert sha(root / name) == manifest[name], ('old report modified', name)
    previous = read(root / 'runs/official_all/summary.json')
    assert comparison['old_summary_sha256'] == sha(root / 'runs/official_all/summary.json')
    for row in previous['rows']:
        assert sha(root / row['plan']) == row['plan_sha256']
        assert sha(root / row['result']) == row['result_sha256']
    official_manifest = read(root / 'reports/rules_manifest.json')['files']
    for name, digest in official_manifest.items():
        assert sha(root / 'references/hackathon_20261030' / name) == digest, name
    v7_manifest = read(root / 'reports/v7_preservation_manifest.json')
    v7_files = v7_manifest.get('files', v7_manifest)
    for name, digest in v7_files.items():
        assert sha(root.parent / 'dag_chain_solver_v7' / name) == digest, name
    # Preserve all keys verbatim in per-case diagnostics. Aggregate only actual
    # numeric/boolean leaves; this remains compatible with additional counters.
    stats = [row['generation'].get('feedback_stats') for row in summary['rows']]
    assert all(isinstance(s, dict) for s in stats), 'missing feedback diagnostics'
    totals, stops = Counter(), Counter()
    def collect(obj, prefix=''):
        for key, value in obj.items():
            full = prefix + key
            if isinstance(value, dict):
                collect(value, full + '.')
            elif type(value) in (int, float, bool):
                totals[full] += value
    for item in stats:
        collect(item)
        if 'stop_reason' in item:
            stops[item['stop_reason']] += 1
    report = dict(version='0.8.1', verified_utc=datetime.now(timezone.utc).isoformat(),
                  source_sha256=summary['source_sha256'], solver_executable_sha256=sha(solver),
                  scope='Scene B singleton Step1/Step2 exact parity; C++ Step3 cycles remain approximate, all final public and synthetic plans checked by unmodified official scorer.',
                  cpp_tests=cpp, cli_generic_checks=generic, graph_contract_checks=int(match[1]),
                  synthetic_official=dict(passed=64, repeat_identity_passed=64,
                                          report=evidence('runs/synthetic_upgrade/report.json')),
                  spill_parity=dict(exact_checks=120, public=40, synthetic=64, adversarial=16,
                                    reused_old_binary_checks=0, report=evidence('runs/spill_differential_upgrade/report.json')),
                  official_public=dict(passed=40, comparison=comparison['summary'],
                                       report=evidence('reports/RESULTS_V8_UPGRADE.json')),
                  feedback_diagnostics=dict(cases=len(stats), numeric_leaf_sums=dict(totals),
                                            stop_reasons=dict(stops),
                                            note='Numeric leaf sums are diagnostic aggregates, not performance scores; per-case values are retained in official summary.'),
                  preservation=dict(v7_unchanged_files=len(v7_files), official_unchanged_files=len(official_manifest),
                                    original_v8_files=len(manifest), snapshot=evidence(backup_name + '/snapshot.zip'),
                                    original_run=evidence('runs/official_all/summary.json'),
                                    original_report=evidence('reports/RESULTS_V8.json')),
                  platform_note='Windows MSVC and MinGW tested; native Linux and older GCC not tested.')
    (root / 'reports/VALIDATION_V8_UPGRADE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({key: report[key] for key in ('version', 'graph_contract_checks', 'official_public', 'feedback_diagnostics')}, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
