"""Measure internal estimate error against the already recorded official results."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--diagnostics', type=Path, default=root/'build_msvc/bin/chain_diagnostics.exe')
    a = p.parse_args()
    actual = json.loads((root/'reports/official_v4_20261030.json').read_text())
    rows = []
    for row in actual['rows']:
        case, cores = row['case'], row['cores']
        graph = root/'references/hackathon_20261030/data'/f'{case}.json'
        plan = root/'runs/final/output_checks_msvc'/f'{case}_n{cores}_v4.json'
        result = subprocess.run([str(a.diagnostics), str(graph), str(cores), '--evaluate-plan', str(plan)],
                                capture_output=True, text=True, check=True, timeout=90)
        estimate = json.loads(result.stdout)
        assert estimate['valid']
        rows.append(dict(case=case, cores=cores, official_cycles=row['cycles'], **estimate,
                         signed_cycle_error_percent=100*(estimate['estimated_cycles']/row['cycles']-1),
                         copy_error_bytes=estimate['route']+estimate['spill']-row['copy_bytes']))
    report = dict(meaning='diagnostic fidelity only, not an official score', rows=rows,
                  mean_absolute_cycle_error_percent=statistics.mean(abs(r['signed_cycle_error_percent']) for r in rows),
                  maximum_absolute_cycle_error_percent=max(abs(r['signed_cycle_error_percent']) for r in rows),
                  exact_transfer_byte_matches=sum(r['copy_error_bytes'] == 0 for r in rows))
    (root/'reports/estimate_fidelity.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'rows'}, indent=2))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
