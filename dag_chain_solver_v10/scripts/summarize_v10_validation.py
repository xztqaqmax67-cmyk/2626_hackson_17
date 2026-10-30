"""Combine completed v10 cumulative benchmarks and exact timing validation."""
from collections import Counter
from datetime import datetime,timezone
import hashlib
import json
from pathlib import Path
import re


def read(p):return json.loads(p.read_text(encoding='utf-8'))
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    root=Path(__file__).resolve().parents[1]
    def evidence(p):return dict(path=str(p.relative_to(root)),sha256=sha(p))
    comparison=read(root/'reports/RESULTS_V10.json');assert comparison['completed'] and len(comparison['rows'])==120
    for name,digest in comparison['source_sha256'].items():assert sha(root/name)==digest,('source changed',name)
    runs={}
    for name,meta in comparison['run_metadata'].items():
        p=Path(meta['run']);assert sha(p)==meta['run_sha256'];run=read(p)
        assert run['completed'] and len(run['rows'])==40 and run['source_sha256']==comparison['source_sha256']
        assert sha(Path(meta['solver_path']))==meta['solver_executable_sha256']
        runs[name]=run
    expected=set(re.findall(r'add_test\(NAME\s+(\w+)',(root/'CMakeLists.txt').read_text(encoding='utf-8')))
    cpp={}
    for compiler in ('msvc','mingw'):
        p=root/f'build_{compiler}/Testing/Temporary/LastTest.log';log=p.read_text(encoding='utf-8',errors='replace')
        tests=re.findall(r'^\d+/\d+ Testing: (\w+)$',log,re.MULTILINE)
        assert len(tests)==len(expected) and set(tests)==expected,(compiler,tests)
        assert log.count('Test Passed.')==len(expected) and 'Test Failed.' not in log
        binaries=[Path(x) for x in re.findall(r'^Command: "([^"]+)"',log,re.MULTILINE)]
        assert len(binaries)==len(expected) and all(p.stat().st_mtime>=x.stat().st_mtime for x in binaries)
        cpp[compiler]=dict(passed=len(tests),tests=tests,log=evidence(p),executable_sha256={x.relative_to(root).as_posix():sha(x) for x in binaries})
    generic=read(root/'runs/validation/summary.json');assert {x['name'] for x in generic}>={'cli','rules','synthetic'}
    assert all(x['exit_code']==0 for x in generic)
    assert 'CLI contract checks passed' in (root/'runs/validation/cli.txt').read_text(encoding='utf-8')
    rules=(root/'runs/validation/rules.txt').read_text(encoding='utf-8');match=re.search(r'(\d+) graph contract checks passed',rules);assert match
    synthetic=read(root/'runs/synthetic/report.json')
    formal_solver=root/'build_msvc/bin/npu_sched.exe'
    assert synthetic['completed'] and synthetic['passed']==64 and synthetic['failed']==0
    assert synthetic['solver_executable_sha256']==sha(formal_solver)
    assert all(x['status']=='ok' and x['repeat_plan_identical'] for x in synthetic['rows'])
    timing_path=root/'runs/timing_differential/report.json';timing=read(timing_path)
    assert timing['completed'] and timing['passed']==120 and timing['failed']==0
    assert Counter(x['group'] for x in timing['rows'])==dict(public=40,synthetic=64,adversarial=16)
    assert timing['metadata']['diagnostics_sha256']==sha(root/'build_msvc/bin/timing_diagnostics.exe')
    assert timing['metadata']['solver_sha256']==sha(formal_solver)
    assert timing['metadata']['checker_sha256']==sha(root/'scripts/check_official_timing.py')
    for name,digest in timing['metadata']['fixture_sources_sha256'].items():assert sha(root/name)==digest
    for name,digest in timing['metadata']['source_sha256'].items():assert sha(root/name)==digest
    exact={(x['case'],x['cores']):x for x in runs['exact']['rows']}
    for row in timing['rows']:
        assert row['status']=='ok'
        folder=timing_path.parent
        assert sha(folder/row['cpp_trace'])==row['cpp_trace_sha256']
        assert sha(folder/row['official_trace'])==row['official_trace_sha256']
        if row['group']=='public':
            scored=exact[row['case'],row['cores']]
            assert row['plan_sha256']==scored['plan_sha256'] and row['input_sha256']==scored['input_sha256']
            assert row['cycles']==scored['cycles']
            assert row['spill_bytes']==scored['data_movement_bytes']['spill_added_copy_bytes']
            assert row['route_bytes']+row['spill_bytes']==scored['data_movement_bytes']['scheduled_copy_bytes']
    preflight_path=root/'runs/timing_preflight_final/report.json';preflight=read(preflight_path)
    assert preflight['completed'] and preflight['passed']==104 and preflight['failed']==0
    assert preflight['by_group']==dict(synthetic=64,adversarial=16,collisions=4,precision=20)
    assert preflight['metadata']['diagnostics_sha256']==timing['metadata']['diagnostics_sha256']
    assert preflight['metadata']['checker_sha256']==timing['metadata']['checker_sha256']
    for name,digest in preflight['metadata']['source_sha256'].items():assert sha(root/name)==digest
    for row in preflight['rows']:
        for kind in ('cpp','official'):
            assert sha(preflight_path.parent/row[kind+'_trace'])==row[kind+'_trace_sha256']
    fuzz_path=root/'runs/timing_review_fuzz_multicore/report.json';fuzz=read(fuzz_path)
    assert len(fuzz['rows'])==240 and fuzz['binary_sha256']==timing['metadata']['diagnostics_sha256']
    assert all(x['mismatch'] is None and x['official_valid']==x['cpp_valid'] for x in fuzz['rows'])
    preserved={}
    for version in ('v7','v8','v9'):
        manifest=read(root/f'reports/{version}_preservation_manifest.json');files=manifest.get('files',manifest)
        for name,digest in files.items():assert sha(root.parent/f'dag_chain_solver_{version}'/name)==digest,(version,name)
        preserved[version+'_unchanged_files']=len(files)
    official=read(root/'reports/rules_manifest.json')['files']
    for name,digest in official.items():assert sha(root/'references/hackathon_20261030'/name)==digest
    preserved['official_unchanged_files']=len(official)
    diagnostics={}
    for variant,run in runs.items():
        totals=Counter()
        def collect(obj,prefix=''):
            items=enumerate(obj) if isinstance(obj,list) else obj.items()
            for k,v in items:
                name=prefix+str(k)
                if isinstance(v,(list,dict)):collect(v,name+'.')
                elif type(v) in (int,float,bool):totals[name]+=v
        for row in run['rows']:
            stats={k:v for k,v in row['generation'].items() if k.endswith('_stats')}
            assert 'v10_stats' in stats,'missing v10 diagnostics'
            collect(stats)
        diagnostics[variant]=dict(cases=40,numeric_leaf_sums=dict(totals))
    result=dict(version='0.10.0',verified_utc=datetime.now(timezone.utc).isoformat(),
                source_sha256=comparison['source_sha256'],formal_solver_sha256=sha(formal_solver),cpp_tests=cpp,
                cli_generic_checks=generic,graph_contract_checks=int(match[1]),
                synthetic_official=dict(passed=64,repeat_identity_passed=64,report=evidence(root/'runs/synthetic/report.json')),
                exact_timing=dict(passed=120,public=40,synthetic=64,adversarial=16,report=evidence(timing_path),
                                  compared=['cycles','route/spill bytes','local cycles','memory peak','pipe orders','memory dependency edges/bytes/previous tensors/positions/kind','local and global per-op start/end']),
                timing_preflight=dict(passed=104,by_group=preflight['by_group'],matched_official_rejections=preflight['matched_official_rejections'],report=evidence(preflight_path)),
                independent_timing_fuzz=dict(passed=240,official_valid=sum(x['official_valid'] for x in fuzz['rows']),
                                             matched_official_rejections=sum(not x['official_valid'] for x in fuzz['rows']),report=evidence(fuzz_path)),
                official_public=dict(passed=120,variants=comparison['variants'],report=evidence(root/'reports/RESULTS_V10.json')),
                diagnostics=diagnostics,preservation=preserved,
                limitations=['Exact port is validated for the supported singleton Scene B representation; this is not proof for all arbitrary inputs.',
                             'Windows MSVC/MinGW tested; native Linux and older GCC not tested.',
                             'exact stage adds exact final selection and permits total-traffic tradeoffs; it is not a timing-model-only ablation.',
                             'Diagnostic counter sums have different units; per-variant times are independent full-process measurements.'])
    save=root/'reports/VALIDATION_V10.json';save.write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps({k:result[k] for k in ('version','graph_contract_checks','exact_timing','preservation')},indent=2))

if __name__=='__main__':main()
