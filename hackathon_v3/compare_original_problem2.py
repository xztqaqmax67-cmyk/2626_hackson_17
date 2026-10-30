#!/usr/bin/env python3
"""Run the original problem2 evaluator against the existing v3 plans.

The original evaluator, original config, input data, solver and saved plans are
never edited. Compatibility results are explicitly distinguished from strict
original-input results. This harness performs no event simulation itself.
"""
import argparse
import configparser
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
DEFAULT_ORIGINAL = HERE.parent / '前辈代码' / 'problem2'
DATA_DIR = HERE.parent / 'hackathon_data'
STAGES = {
    'strict_matched': 'PRIMARY: unchanged input and evaluator; independent config using current numeric settings',
    'compat_matched': 'PRIMARY compatibility diagnostic: COPY cycles -1 to 0 in copies; current numeric settings; unchanged evaluator',
    'strict_original': 'historical diagnostic: unchanged original inputs, original old config and evaluator',
    'compat_original': 'historical diagnostic: COPY cycles -1 to 0 in copies; original old config and evaluator',
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def classify(message):
    if 'cycles must be an integer' in message:
        return 'unsupported_negative_copy_cycles'
    if 'pipe must be one of' in message:
        return 'unsupported_PIPE_FIX'
    if 'deadlock' in message.lower() or 'Step3SchedulingError' in message:
        return 'scheduler_deadlock_or_memory_failure'
    if 'cycle' in message.lower():
        return 'dependency_cycle'
    if 'capacity' in message.lower() or 'Step2SchedulingError' in message:
        return 'memory_capacity_or_spill_failure'
    return 'evaluation_error'


def run_evaluator(script, graph, plan, config, directory, timeout):
    directory.mkdir(parents=True, exist_ok=True)
    result_path = directory / 'result.json'
    if result_path.exists():
        raise ValueError(f'refusing to reuse an existing result: {result_path}')
    command = [sys.executable, '-B', str(script), str(graph)]
    if plan is not None:
        command.append(str(plan))
    command += ['--config', str(config), '-o', str(result_path),
                '--trace-output', str(directory / 'trace.json'),
                '--log-output', str(directory / 'evaluator.log')]
    save(directory / 'command.json', command)
    start = time.perf_counter()
    try:
        proc = subprocess.run(command, capture_output=True, encoding='utf-8', errors='replace', timeout=timeout,
                              env={**os.environ, 'PYTHONIOENCODING': 'utf-8'})
        elapsed = time.perf_counter() - start
        (directory / 'stdout.txt').write_text(proc.stdout, encoding='utf-8')
        (directory / 'stderr.txt').write_text(proc.stderr, encoding='utf-8')
        if proc.returncode:
            error = (proc.stderr or proc.stdout).strip()
            row = {'status': classify(error), 'returncode': proc.returncode,
                   'evaluation_seconds': elapsed, 'error': error[-6000:]}
        elif not result_path.is_file():
            row = {'status': 'missing_output', 'returncode': 0, 'evaluation_seconds': elapsed,
                   'error': 'evaluator returned zero without producing a result'}
        else:
            result = load(result_path)
            movement = result.get('data_movement_bytes', {})
            row = {'status': 'ok', 'returncode': 0, 'evaluation_seconds': elapsed,
                   'makespan': result['makespan'],
                   'ddr_bytes': movement.get('scheduled_copy_bytes'), 'error': ''}
    except subprocess.TimeoutExpired as exc:
        for name, output in (('stdout', exc.stdout), ('stderr', exc.stderr)):
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            (directory / f'{name}.txt').write_text(output or '', encoding='utf-8')
        row = {'status': 'timeout', 'returncode': None, 'evaluation_seconds': time.perf_counter() - start,
               'error': f'evaluator exceeded {timeout} seconds'}
    save(directory / 'status.json', row)
    return row


def prepare(original, out):
    manifest_path = out / 'manifest.json'
    sources = sorted((original / 'official_code').glob('*.py')) + [original / 'config.txt']
    if manifest_path.is_file():
        manifest = load(manifest_path)
        for entry in manifest['protected_files']:
            if digest(Path(entry['path'])) != entry['sha256']:
                raise ValueError(f'source changed since snapshot: {entry["path"]}')
        manifest['stages'] = STAGES
        manifest['primary_config'] = 'config_matched_current.txt (user requested current settings when conflicting)'
        save(manifest_path, manifest)
        return manifest
    out.mkdir(parents=True, exist_ok=True)
    local = load(HERE / 'report_v3_all/summary.json')
    protected = sources + sorted(DATA_DIR.glob('case_*.json'))
    protected += sorted((HERE / 'report_v3_all/plans').glob('*.json'))
    protected += [HERE / 'solver.cpp', HERE / 'score.py', HERE / 'config_LOCAL_ASSUMPTIONS.txt',
                  HERE / 'report_v3_all/summary.json', HERE / 'report_v3_all/cases.csv']
    manifest = {'original_problem2': str(original), 'python': sys.executable,
                'protected_files': [{'path': str(p), 'sha256': digest(p)} for p in protected],
                'local_summary': local['summary'], 'cases': {},
                'stages': STAGES,
                'primary_config': 'config_matched_current.txt (user requested current settings when conflicting)'}
    shutil.copyfile(original / 'config.txt', out / 'config_original.txt')
    cfg = configparser.ConfigParser(inline_comment_prefixes=('#',))
    cfg.read(HERE / 'config_LOCAL_ASSUMPTIONS.txt', encoding='utf-8-sig')
    text = (original / 'config.txt').read_text(encoding='utf-8')
    # Keep old parser's whitespace syntax and required scene-A baseline settings.
    for key, value in [('L1', cfg.getint('capacity', 'L1')),
                       ('UB', cfg.getint('capacity', 'UB')),
                       ('bandwidth', cfg.getint('bandwidth', 'bandwidth')),
                       ('cross_core_copy_delay_cycles', cfg.getint('multicore_scene_b', 'cross_core_copy_delay_cycles'))]:
        text, count = re.subn(r'^' + re.escape(key) + r'\s+\d+\s*$', f'{key} {value}', text, flags=re.M)
        if count != 1:
            raise ValueError(f'cannot update config copy: {key}')
    text = text.replace('# 竞赛结果统一采用以下固定评估值，不得修改。',
                        '# Comparison-only config copy: current Hackathon numeric settings; NOT original contest config.')
    (out / 'config_matched_current.txt').write_text(text, encoding='utf-8')
    for graph in sorted(DATA_DIR.glob('case_*.json')):
        raw = load(graph)
        negative_copies = [o['id'] for o in raw['ops'] if o['op'] in ('COPY_IN', 'COPY_OUT') and o['cycles'] == -1]
        unsupported = sorted({o['pipe'] for o in raw['ops']} - {'PIPE_MTE2', 'PIPE_MTE3', 'PIPE_M', 'PIPE_V'})
        negative_ids = set(negative_copies)
        for o in raw['ops']:
            if o['id'] in negative_ids:
                o['cycles'] = 0
        save(out / 'inputs_copy_cycles_zero' / graph.name, raw)
        snapshot = out / 'plans' / f'{graph.stem}_multicore_res.json'
        snapshot.parent.mkdir(exist_ok=True)
        shutil.copyfile(HERE / 'report_v3_all/plans' / snapshot.name, snapshot)
        manifest['cases'][graph.stem] = {
            'input_sha256': digest(graph), 'plan_sha256': digest(snapshot),
            'compat_input_sha256': digest(out / 'inputs_copy_cycles_zero' / graph.name),
            'normalized_copy_count': len(negative_copies), 'unsupported_pipes': unsupported,
        }
    save(out / 'local_v3_summary_snapshot.json', local)
    save(manifest_path, manifest)
    return manifest


def collect(out, manifest):
    local = {r['case']: r for r in load(out / 'local_v3_summary_snapshot.json')['cases']}
    rows, stages = [], {}
    for stage in manifest['stages']:
        stage_rows = []
        for case in manifest['cases']:
            multi_path = out / stage / case / 'multi/status.json'
            if not multi_path.is_file():
                continue
            multi = load(multi_path)
            single_path = out / stage / case / 'single/status.json'
            single = load(single_path) if single_path.is_file() else {}
            row = {'stage': stage, 'case': case, 'multi_status': multi['status'],
                   'single_status': single.get('status', 'not_run'),
                   'original_multicore_makespan': multi.get('makespan'),
                   'original_singlecore_makespan': single.get('makespan'),
                   'original_ddr_bytes': multi.get('ddr_bytes'),
                   'original_speedup': (single['makespan'] / multi['makespan']
                                        if multi['status'] == single.get('status') == 'ok' else None),
                   'local_v3_multicore_makespan': local[case]['multicore_makespan'],
                   'local_v3_singlecore_makespan': local[case]['singlecore_makespan'],
                   'local_v3_speedup': local[case]['speedup'],
                   'local_v3_ddr_bytes': local[case]['ddr_bytes'],
                   'multi_error': multi.get('error', ''), 'single_error': single.get('error', '')}
            rows.append(row)
            stage_rows.append(row)
        successful = [r for r in stage_rows if r['multi_status'] == 'ok']
        both = [r for r in stage_rows if r['original_speedup'] is not None]
        stages[stage] = {'description': manifest['stages'][stage],
                         'attempted_cases': len(stage_rows), 'multicore_success': len(successful),
                         'cases_with_speedup': len(both),
                         'full_suite_average_speedup': statistics.mean(r['original_speedup'] for r in both)
                         if len(both) == len(manifest['cases']) else None,
                         'full_suite_ddr_bytes': sum(r['original_ddr_bytes'] for r in successful)
                         if len(successful) == len(manifest['cases']) else None,
                         'successful_subset_cases': [r['case'] for r in both],
                         'successful_subset_average_speedup': statistics.mean(r['original_speedup'] for r in both) if both else None,
                         'same_subset_local_v3_average_speedup': statistics.mean(r['local_v3_speedup'] for r in both) if both else None}
    save(out / 'comparison.json', {'stages': stages, 'cases': rows,
                                 'note': 'Compatibility stages use altered input copies. Matching numeric settings does not match simulator semantics. No solver was rerun.'})
    if rows:
        with (out / 'comparison.csv').open('w', newline='', encoding='utf-8-sig') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    primary = []
    for row in rows:
        if row['stage'] != 'compat_matched':
            continue
        item = {key: value for key, value in row.items() if key not in ('stage', 'multi_error', 'single_error')}
        item['makespan_difference_percent'] = (100 * (row['original_multicore_makespan'] / row['local_v3_multicore_makespan'] - 1)
                                               if row['multi_status'] == 'ok' else None)
        item['ddr_difference_bytes'] = (row['original_ddr_bytes'] - row['local_v3_ddr_bytes']
                                        if row['multi_status'] == 'ok' else None)
        primary.append(item)
    if primary:
        with (out / 'current_config_comparison.csv').open('w', newline='', encoding='utf-8-sig') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(primary[0]))
            writer.writeheader()
            writer.writerows(primary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path, default=DEFAULT_ORIGINAL)
    parser.add_argument('--out', type=Path, default=HERE / 'runs' / 'original_problem2')
    parser.add_argument('--stage', choices=list(STAGES), required=True)
    parser.add_argument('--cases', nargs='*')
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--defer-summary', action='store_true', help='defer shared summary while an independent stage is running')
    args = parser.parse_args()
    original, out = args.original.resolve(), args.out.resolve()
    manifest = prepare(original, out)
    cases = args.cases or list(manifest['cases'])
    for case in cases:
        if case not in manifest['cases']:
            parser.error(f'unknown case {case}')
        directory = out / args.stage / case
        graph = (DATA_DIR if args.stage.startswith('strict_') else out / 'inputs_copy_cycles_zero') / f'{case}.json'
        config = (out / 'config_matched_current.txt' if args.stage.endswith('_matched') else original / 'config.txt')
        plan = out / 'plans' / f'{case}_multicore_res.json'
        for mode in (['multi'] if args.stage.startswith('strict_') else ['multi', 'single']):
            if (directory / mode / 'status.json').is_file():
                continue
            script = original / 'official_code' / ('multicore_cut_evaluate_problem_2.py' if mode == 'multi' else 'singlecore_evaluate.py')
            result = run_evaluator(script, graph, plan if mode == 'multi' else None,
                                   config, directory / mode, args.timeout)
            print(f"[{args.stage}/{case}/{mode}] {result['status']} makespan={result.get('makespan')} DDR={result.get('ddr_bytes')} ({result['evaluation_seconds']:.2f}s)", flush=True)
            if not args.defer_summary:
                collect(out, manifest)
    unchanged = all(digest(Path(e['path'])) == e['sha256'] for e in manifest['protected_files'])
    save(out / 'integrity.json', {'all_protected_files_unchanged': unchanged,
                                  'protected_file_count': len(manifest['protected_files'])})
    if not unchanged:
        raise ValueError('a protected source changed during evaluation')
    if not args.defer_summary:
        collect(out, manifest)


if __name__ == '__main__':
    main()
