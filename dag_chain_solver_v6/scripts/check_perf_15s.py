"""Compare fixed-work seed search before/after runtime changes; no timed search or scoring.

Run only after both Release builds are complete and no other benchmark is running.
Every process performs the same deterministic solve(), independently of 15/30-second
budgets. Whole-process wall time includes Graph cache construction and plan output;
the diagnostic's narrower solve-only time is retained as a secondary measurement.
"""

import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_hashes(root):
    paths = [root / 'CMakeLists.txt', root / 'tests/diagnostics.cpp',
             *sorted((root / 'src').glob('*'))]
    return {p.relative_to(root).as_posix(): sha(p) for p in paths if p.is_file()}


def build_settings(executable):
    path = executable.parent.parent / 'CMakeCache.txt'
    wanted = {'CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER',
              'CMAKE_CXX_FLAGS_RELEASE', 'CMAKE_GENERATOR'}
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if '=' not in line or ':' not in line or line.startswith(('#', '//')):
            continue
        name, value = line.split('=', 1)
        name = name.split(':', 1)[0]
        if name in wanted:
            result[name] = value
    assert set(result) == wanted, f'Missing build settings: {path}'
    assert result['CMAKE_BUILD_TYPE'] == 'Release', path
    return result


def main():
    root = Path(__file__).resolve().parents[1]
    snapshot = root / 'snapshots/v6_0_6_0_before_perf_15s'
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--old', type=Path,
                        default=snapshot / 'build_perf/bin/chain_diagnostics.exe')
    parser.add_argument('--new', type=Path,
                        default=root / 'build_mingw/bin/chain_diagnostics.exe')
    parser.add_argument('--out', type=Path, default=root / 'runs/perf_15s')
    parser.add_argument('--cases', nargs='+', default=['case_00', 'case_05', 'case_08'])
    parser.add_argument('--cores', type=int, choices=range(2, 6), default=5)
    parser.add_argument('--repetitions', type=int, default=3)
    args = parser.parse_args()
    assert args.repetitions >= 1
    assert len(set(args.cases)) == len(args.cases)
    assert all(case in {f'case_{i:02}' for i in range(10)} for case in args.cases)
    out = args.out.resolve()
    assert out.is_relative_to(root), 'Keep measurement artifacts inside this project'
    assert not out.exists() or not any(out.iterdir()), 'Use a fresh output directory'
    out.mkdir(parents=True, exist_ok=True)
    binaries = {'old': args.old.resolve(), 'new': args.new.resolve()}
    roots = {'old': snapshot, 'new': root}
    settings = {name: build_settings(exe) for name, exe in binaries.items()}
    assert settings['old'] == settings['new'], 'Compiler/build settings differ'
    manifest = json.loads((snapshot / 'manifest.json').read_text(encoding='utf-8'))
    for name, digest in manifest.items():
        assert sha(snapshot / name) == digest, f'Preserved snapshot changed: {name}'
    rules = json.loads((root / 'reports/rules_manifest.json').read_text(encoding='utf-8'))
    graphs = {case: root / f'references/hackathon_20261030/data/{case}.json'
              for case in args.cases}
    for case, graph in graphs.items():
        assert sha(graph) == rules['files'][f'data/{case}.json']
    assert (snapshot / 'tests/diagnostics.cpp').read_bytes() == (root / 'tests/diagnostics.cpp').read_bytes()
    report = dict(
        measurement='fixed_work_deterministic_seed_search', cores=args.cores,
        repetitions=args.repetitions, cases=args.cases, completed=False,
        definitions=dict(
            workload='chain_diagnostics calls solve(), not solve_for_budget(); search limits in seconds are not active.',
            primary_timing='Whole process wall time including parsing, immutable cache construction, solve, and output.',
            secondary_timing='Diagnostic seconds field: solve only, excludes parsing and output.',
            equivalence='Exact emitted plan bytes and all diagnostic fields other than seconds, across versions and repetitions.',
            aggregation='Sum of per-case median times; ratio old/new. Not an arithmetic mean of speedup ratios.',
            scope='Covers seed search comparator and MemoryOrder changes. Does not measure timed anytime neighborhood generation or prove arbitrary-input correctness.',
            timing_limit='Three serial repetitions by default, alternating old/new order. No statistical significance claim.',
            scoring='No official scorer is invoked; reported estimates are internal values.'),
        executable_sha256={name: sha(exe) for name, exe in binaries.items()},
        executable_paths={name: str(exe) for name, exe in binaries.items()},
        source_sha256={name: source_hashes(folder) for name, folder in roots.items()},
        input_sha256={case: sha(graph) for case, graph in graphs.items()},
        build_settings=settings, runs=[], rows=[], errors=[])
    saved = out / 'report.json'

    def save():
        saved.write_text(json.dumps(report, indent=2), encoding='utf-8')

    save()
    canonical = {}
    try:
        for case_index, (case, graph) in enumerate(graphs.items()):
            for repetition in range(args.repetitions):
                order = ['old', 'new'] if (case_index + repetition) % 2 == 0 else ['new', 'old']
                for name in order:
                    folder = out / case / f'rep_{repetition + 1}_{name}'
                    folder.mkdir(parents=True)
                    plan = folder / 'plan.json'
                    command = [str(binaries[name]), str(graph), str(args.cores), str(plan)]
                    row = dict(case=case, variant=name, repetition=repetition + 1,
                               pair_order=order, command=command, status='running')
                    report['runs'].append(row)
                    save()
                    started = time.perf_counter()
                    result = subprocess.run(command, capture_output=True, timeout=120)
                    row.update(process_wall_seconds=time.perf_counter() - started,
                               exit_code=result.returncode)
                    (folder / 'stdout.txt').write_bytes(result.stdout)
                    (folder / 'stderr.txt').write_bytes(result.stderr)
                    row['artifacts'] = str(folder.relative_to(root))
                    if result.returncode:
                        row['status'] = 'error'
                        row['error'] = result.stderr.decode('utf-8', errors='replace')
                        save()
                        raise RuntimeError(f'{case} {name} failed with exit code {result.returncode}')
                    diagnostic = json.loads(result.stdout.decode('utf-8'))
                    row['solve_seconds'] = diagnostic.pop('seconds')
                    row['diagnostic'] = diagnostic
                    row['plan_sha256'] = sha(plan)
                    row['plan'] = str(plan.relative_to(root))
                    fingerprint = (plan.read_bytes(), diagnostic)
                    if case not in canonical:
                        canonical[case] = fingerprint
                    assert fingerprint == canonical[case], f'Plan/diagnostic mismatch: {case} {name} repetition {repetition + 1}'
                    row['status'] = 'ok'
                    row['matches_all_prior_runs'] = True
                    save()
                    print(f'{case} rep {repetition + 1} {name}: {row["process_wall_seconds"]:.3f}s, exact parity', flush=True)
        for case in args.cases:
            row = dict(case=case, cores=args.cores, exact_parity=True)
            for name in binaries:
                subset = [r for r in report['runs'] if r['case'] == case and r['variant'] == name]
                row[name] = {
                    key: statistics.median(r[key] for r in subset)
                    for key in ['process_wall_seconds', 'solve_seconds']}
                row[name]['plan_sha256'] = subset[0]['plan_sha256']
                row[name]['diagnostic'] = subset[0]['diagnostic']
            row['process_speedup'] = row['old']['process_wall_seconds'] / row['new']['process_wall_seconds']
            row['solve_speedup'] = row['old']['solve_seconds'] / row['new']['solve_seconds']
            report['rows'].append(row)
        for name, exe in binaries.items():
            assert sha(exe) == report['executable_sha256'][name], 'Executable changed during measurement'
            assert source_hashes(roots[name]) == report['source_sha256'][name], 'Source changed during measurement'
        totals = {name: {key: sum(r[name][key] for r in report['rows'])
                         for key in ['process_wall_seconds', 'solve_seconds']} for name in binaries}
        report['summary'] = dict(
            median_time_sums=totals,
            process_speedup=totals['old']['process_wall_seconds'] / totals['new']['process_wall_seconds'],
            solve_speedup=totals['old']['solve_seconds'] / totals['new']['solve_seconds'],
            all_plans_and_diagnostics_identical=True)
        report['summary']['process_time_reduction_percent'] = 100 * (1 - 1 / report['summary']['process_speedup'])
        report['completed'] = True
        save()
    except Exception as error:
        if report['runs'] and report['runs'][-1]['status'] == 'running':
            report['runs'][-1]['status'] = 'error'
            report['runs'][-1]['error'] = f'{type(error).__name__}: {error}'
        report['errors'].append(f'{type(error).__name__}: {error}')
        save()
        raise

    reports = root / 'reports'
    (reports / 'PERF_15S.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    summary = report['summary']
    lines = ['# v6 實作效率：固定工作量對照', '',
             f'修改前 0.6.0 與目前版使用相同 MinGW Release 設定；{len(args.cases)} 個用例 × 每版 {args.repetitions} 次，全部串行，交替執行前後版本。',
             '所有輸出方案逐位元相同，診斷中的估價、搬運量、候選數、標籤及核心配額均完全相同。', '',
             '**量測範圍是固定工作量的原種子搜尋 `solve()`，沒有啟動 15／30 秒追加搜尋，也沒有執行官方評分。**',
             '因此這裡的速度不能代表整個計時搜尋或官方 NPU cycles 的改善。追加搜尋的比較器另由單元／合法性測試驗證。', '',
             '主要計時使用整個程序牆鐘時間，包含讀圖、快取建構和寫出，避免把工作移到讀圖階段後誤算為收益。',
             'solve 秒數只作補充，不含讀圖和寫出；均採每例中位數。', '',
             '| 用例 | 修改前程序秒 | 修改後程序秒 | 程序速度比 | 修改前 solve 秒 | 修改後 solve 秒 |',
             '|---|---:|---:|---:|---:|---:|']
    for row in report['rows']:
        lines.append(f'| {row["case"]} | {row["old"]["process_wall_seconds"]:.4f} | {row["new"]["process_wall_seconds"]:.4f} | {row["process_speedup"]:.4f}× | {row["old"]["solve_seconds"]:.4f} | {row["new"]["solve_seconds"]:.4f} |')
    lines += ['',
              f'逐例中位數合計：程序耗時 {totals["old"]["process_wall_seconds"]:.4f} → {totals["new"]["process_wall_seconds"]:.4f} 秒；速度比 **{summary["process_speedup"]:.4f}×**，耗時變化 **{-summary["process_time_reduction_percent"]:+.3f}%**。',
              '速度比是合計耗時之比，不是逐例速度比的算術平均。大於 1 表示加速；小於 1 的退步同樣保留。',
              '這是本機少量重複測量，沒有宣稱統計顯著性或任意輸入都能取得同樣收益；背景負載仍可能影響時間。', '',
              f'原始各次紀錄、stdout/stderr 與方案位於 `{out.relative_to(root).as_posix()}/`。',
              '精確數值、執行檔／來源／輸入雜湊與編譯設定見 [PERF_15S.json](PERF_15S.json)。', '']
    (reports / 'PERF_15S_zh.md').write_text('\n'.join(lines), encoding='utf-8')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    main()
