"""验证中文路径、场景 B 开发入口、限时搜索及原版/C++ 检查点继承。"""
import os
import shutil
import tempfile
import time
from compare_multicore import fixture
from compare_original import *
from problem2 import inheritance

BIN = Path(os.environ.get('PROBLEM2_BIN', BUILD / 'bin'))
SUFFIX = '.exe' if sys.platform == 'win32' else ''


def run(executable, args, *, env=None):
    if not SUFFIX:
        executable = str(executable).removesuffix('.exe')
    process = subprocess.run([str(executable), *map(str, args)], text=True, encoding='utf-8',
                             capture_output=True, timeout=25, env=env)
    assert process.returncode == 0, process.stdout + process.stderr
    return process


def main():
    directory = Path(tempfile.mkdtemp(prefix='集成 验证_', dir=BUILD))
    graph = fixture(19, 16)
    graph_path = directory / '中文 计算图.json'
    config = directory / 'config.txt'
    graph_path.write_text(json.dumps(graph), encoding='utf-8')
    shutil.copyfile(ROOT / '前辈代码_cpp/problem2/config.txt', config)
    for cores in (2, 3):
        result_dir, work = directory / f'结果_{cores}', directory / f'工作_{cores}'
        args = [graph_path, '-n', cores, '--config', config, '--time-limit', 6,
                '--feasibility-grace', 0, '--output-dir', result_dir, '--work-dir', work,
                '--export-evaluation']
        report = json.loads(run(BIN / 'problem2.exe', args).stdout)
        assert report['status'] == 'complete', report
        plan = json.loads((result_dir / '中文 计算图_multicore_res.json').read_text(encoding='utf-8'))
        actual = json.loads((result_dir / '中文 计算图_problem_2_res.json').read_text(encoding='utf-8'))
        expected = evaluate_scene_b(graph, plan, 60, CAPACITY, 1000)
        expected.update(input_graph=graph_path.name, input_plan='中文 计算图_multicore_res.json')
        assert not difference(normalized(expected), actual), difference(normalized(expected), actual)
        assert (result_dir / '中文 计算图_problem_2_trace.json').is_file()
        print(f'PASS solver {cores} cores / original reevaluation', flush=True)

    checkpoint = directory / '工作_2/best_checkpoint.json'
    for kind in ('cpp', 'python'):
        inherited = checkpoint
        if kind == 'python':
            value = json.loads(checkpoint.read_text(encoding='utf-8'))
            value['problem_instance'] = inheritance.instance_fingerprint(graph_path, config)
            inherited = directory / '原版检查点.json'
            inherited.write_text(json.dumps(value), encoding='utf-8')
        work = directory / f'继承_{kind}'
        run(BIN / 'problem2.exe', [graph_path, '-n', 3, '--time-limit', 5, '--feasibility-grace', 0,
            '--inherit-checkpoint', inherited, '--work-dir', work,
            '-o', directory / f'继承方案_{kind}.json'])
        metrics = json.loads((work / 'metrics.json').read_text(encoding='utf-8'))
        assert metrics['inherited_validation']['status'] == 'verified', metrics
        print(f'PASS inherit {kind} checkpoint / revalidated', flush=True)

    plan_path = directory / '随机 方案.json'
    run(BIN / 'stub_multicore_cut_and_schedule.exe', [graph_path, '-n', 3, '--seed', 7,
        '--min-subgraph-size', 1, '--max-subgraph-size', 3, '-o', plan_path])
    plan = json.loads(plan_path.read_text(encoding='utf-8'))
    assert not difference(normalized(generate_multicore_plan(graph, num_cores=3, seed=7,
        min_subgraph_size=1, max_subgraph_size=3)), plan)
    result_path = directory / '独立评估_2.json'
    run(BIN / 'multicore_cut_evaluate_problem_2.exe', [graph_path, plan_path, '-o', result_path])
    actual = json.loads(result_path.read_text(encoding='utf-8'))
    expected = evaluate_scene_b(graph, plan, 60, CAPACITY, 1000)
    expected.update(input_graph=graph_path.name, input_plan=plan_path.name)
    assert not difference(normalized(expected), actual), difference(normalized(expected), actual)
    print('PASS scene B evaluator CLI / Unicode paths', flush=True)

    # 求解器只接受当前提交要求的 2..5 核；单核入口已移除。
    for executable in ('problem2', 'npu_sched'):
        target = directory / f'{executable}_单核.json'
        rejected = subprocess.run([str(BIN/(executable+SUFFIX)), str(graph_path), '-n', '1',
            '-o', str(target)], capture_output=True, timeout=10)
        assert rejected.returncode != 0 and not target.exists(), executable
    print('PASS both solvers reject one-core mode', flush=True)

    original_path = os.environ['PATH']
    try:
        # Windows CreateProcess 在父进程的 PATH 中定位可执行文件。
        os.environ['PATH'] = str(BIN) + os.pathsep + original_path
        run('problem2.exe', [graph_path, '-n', 2, '--time-limit', 5, '-o', directory/'PATH启动.json'])
    finally:
        os.environ['PATH'] = original_path
    # 严格解析错误不能被静默接受；已有的目标文件不得因此被覆盖。
    invalid = directory / '重复键.json'
    invalid.write_text('{"ops":[],"ops":[],"tensors":[],"edges":[]}', encoding='utf-8')
    protected = directory / '保留.json'
    protected.write_text('unchanged', encoding='utf-8')
    rejected = subprocess.run([str(BIN/('problem2'+SUFFIX)), str(invalid), '-n', '2', '--time-limit', '3',
        '-o', str(protected)], capture_output=True, timeout=10)
    assert rejected.returncode != 0 and protected.read_text(encoding='utf-8') == 'unchanged'
    print('PASS PATH launch / invalid input preserves existing output', flush=True)

    # 较大的真实图验证父进程墙钟上限。保留原始 COPY 占位值。
    large = json.loads((ROOT/'hackathon_data/case_00.json').read_text(encoding='utf-8'))
    large_path = directory/'限时计算图.json'
    large_path.write_text(json.dumps(large), encoding='utf-8')
    target = directory/'限时方案.json'
    started = time.monotonic()
    limited = subprocess.run([str(BIN/('problem2'+SUFFIX)), str(large_path), '-n', '3',
        '--time-limit', '2', '--feasibility-grace', '0', '-o', str(target)],
        capture_output=True, text=True, encoding='utf-8', timeout=6)
    assert time.monotonic() - started < 5
    if limited.returncode == 0:
        limited_report = json.loads(limited.stdout)
        plan = json.loads(target.read_text(encoding='utf-8'))
        score = evaluate_scene_b(large, plan, 60, CAPACITY, 1000)
        assert score['makespan'] == limited_report['makespan']
        assert score['data_movement_bytes'] == limited_report['data_movement_bytes']
    else:
        assert not target.exists(), '没有完整已验证方案时，不得伪造结果文件'
    print('PASS 2-second limit / output verified or clean failure', flush=True)
    print('ALL WORKFLOW CHECKS PASSED', flush=True)


if __name__ == '__main__':
    main()
