"""以原版官方 Python 为准复评已有方案，并比较 C++ 内部评分的完整结果。"""
import argparse
import csv
import time
from official_reference import *


def score(command, request, timeout):
    started = time.perf_counter()
    process = subprocess.run(command, input=json.dumps(request), text=True,
                             encoding='utf-8', capture_output=True, timeout=timeout)
    result = json.loads(process.stdout)
    if process.returncode or 'error' in result:
        raise RuntimeError(result.get('error', process.stderr))
    return result, time.perf_counter() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plans', type=Path, required=True,
                        help='含 <图名>_multicore_res.json 的目录')
    parser.add_argument('--inputs', type=Path, default=OFFICIAL_ROOT / 'data')
    parser.add_argument('--runner', type=Path, default=RUNNER)
    parser.add_argument('--before-runner', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--cases', nargs='+')
    parser.add_argument('--timeout', type=float, default=600)
    args = parser.parse_args()
    manifest = verify_official_sources()
    args.out.mkdir(parents=True, exist_ok=True)
    from evaluation_validation import read_evaluation_config
    from multicore_cut_evaluate_problem_2 import read_scene_b_config
    config = OFFICIAL_ROOT / 'data/config.txt'
    hardware = read_evaluation_config(config)
    delay = read_scene_b_config(config)['cross_core_copy_delay_cycles']
    runner_hash = hashlib.sha256(args.runner.read_bytes()).hexdigest()
    report = dict(reference='Unmodified hackathon.zip official evaluator',
                  archive_sha256=manifest['archive_sha256'], runner_sha256=runner_hash,
                  capacity=hardware['capacity'], bandwidth=hardware['bandwidth'], delay=delay,
                  plans=str(args.plans.resolve()), cases=[])
    if args.before_runner:
        report['before_runner_sha256'] = hashlib.sha256(args.before_runner.read_bytes()).hexdigest()
    graphs = sorted(args.inputs.glob('*.json'))
    if args.cases:
        available = {p.stem for p in graphs}
        if set(args.cases) - available:
            parser.error('unknown cases: ' + ', '.join(sorted(set(args.cases) - available)))
        graphs = [p for p in graphs if p.stem in args.cases]
    if not graphs:
        parser.error('no input graphs')
    for graph_path in graphs:
        row = dict(case=graph_path.stem, full_result_equal=False)
        try:
            graph_bytes = graph_path.read_bytes()
            plan_path = args.plans / f'{graph_path.stem}_multicore_res.json'
            plan_bytes = plan_path.read_bytes()
            graph, plan = json.loads(graph_bytes), json.loads(plan_bytes)
            row.update(cores=len(plan['core_schedules']), input_sha256=hashlib.sha256(graph_bytes).hexdigest(),
                       plan_sha256=hashlib.sha256(plan_bytes).hexdigest())
            request = dict(action='scene_b', graph=graph, plan=plan, capacity=hardware['capacity'],
                           bandwidth=hardware['bandwidth'], delay=delay)
            official, seconds = score([sys.executable, str(Path(__file__).with_name('official_runner.py'))],
                                       request, args.timeout)
            row.update(official_makespan=official['makespan'], official_seconds=seconds,
                       official_data_movement_bytes=official['data_movement_bytes'])
            if hashlib.sha256(args.runner.read_bytes()).hexdigest() != runner_hash:
                raise RuntimeError('C++ runner changed during evaluation')
            internal, seconds = score([str(args.runner)], request, args.timeout)
            diff = difference(official, internal)
            row.update(internal_makespan=internal['makespan'], internal_seconds=seconds,
                       full_result_equal=diff is None, difference=diff,
                       internal_data_movement_bytes=internal['data_movement_bytes'])
            if diff:
                for name, result in [('official', official), ('internal', internal)]:
                    (args.out / f'{graph_path.stem}_{name}.json').write_text(json.dumps(result), encoding='utf-8')
            if args.before_runner:
                try:
                    before, seconds = score([str(args.before_runner)], request, args.timeout)
                    row.update(before_makespan=before['makespan'], before_seconds=seconds,
                               before_data_movement_bytes=before['data_movement_bytes'],
                               before_difference=difference(official, before),
                               makespan_delta=official['makespan'] - before['makespan'],
                               makespan_delta_percent=100*(official['makespan'] / before['makespan'] - 1)
                               if before['makespan'] else None)
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    row['before_error'] = str(error)
            assert graph_path.read_bytes() == graph_bytes and plan_path.read_bytes() == plan_bytes
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            row['error'] = str(error)
        report['cases'].append(row)
        report['matched'] = sum(r['full_result_equal'] for r in report['cases'])
        (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        print(json.dumps(row, ensure_ascii=False), flush=True)
    fields = ['case', 'cores', 'before_makespan', 'official_makespan', 'internal_makespan',
              'makespan_delta', 'makespan_delta_percent', 'full_result_equal', 'error']
    with (args.out / 'comparison.csv').open('w', newline='', encoding='utf-8-sig') as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(report['cases'])
    print(f"Full official-result matches: {report['matched']}/{len(report['cases'])}", flush=True)
    if report['matched'] != len(report['cases']):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
