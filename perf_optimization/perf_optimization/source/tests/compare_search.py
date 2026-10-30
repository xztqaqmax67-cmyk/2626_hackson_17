"""固定评估次数对照搜索轨迹；不把机器速度差异混入算法验证。"""
import contextlib
import io
import tempfile
from types import SimpleNamespace
from compare_multicore import branch_fixture
from compare_original import *
from problem2 import search


def main():
    # Windows 没有 SIGALRM。仅在测试中替换原版的闹钟外壳；算法仍使用原函数，
    # 且以 max_evaluations 结束。此测试不用于证明 Python 的墙钟中断行为。
    original_signal = search.signal
    search.signal = SimpleNamespace(SIGALRM=0, ITIMER_REAL=0, signal=lambda *a: None,
                                    setitimer=lambda *a: None)
    try:
        for seed in (12, 17):
            for limit in (1, 6, 24, 60):
                directory = Path(tempfile.mkdtemp(prefix='search_', dir=BUILD))
                graph = branch_fixture(seed, 40)
                graph_path = directory / 'graph.json'
                graph_path.write_text(json.dumps(graph), encoding='utf-8')
                config = ROOT / '前辈代码_cpp/problem2/config.txt'
                with contextlib.redirect_stdout(io.StringIO()):
                    expected = search.solve(graph_path, 3, config, directory/'python',
                        time_limit=120, max_evaluations=limit, export_results=False)
                request = dict(action='solve', graph=graph, capacity=CAPACITY, graph_path=str(graph_path),
                    config_path=str(config), output=str(directory/'cpp'), max_evaluations=limit, cores=3)
                result = subprocess.run([str(RUNNER)], input=json.dumps(request), text=True,
                    encoding='utf-8', capture_output=True, timeout=130)
                assert result.returncode == 0, result.stdout + result.stderr
                actual = json.loads(result.stdout.splitlines()[-1])
                if limit > 24:
                    # 路线选择使用实际耗时，进入轮转阶段后不要求两种语言走同一轨迹。
                    # 仍以原算法独立验证 C++ 最终方案的完整评分结果。
                    from problem2 import official
                    checkpoint = json.loads((directory/'cpp/best_checkpoint.json').read_text())
                    verified = official.evaluate(graph, checkpoint['plan'], official.load_settings(config))
                    assert verified['status'] == 'ok'
                    assert not difference(normalized(verified['official_result']), checkpoint['official_result'])
                    assert actual['solution_available'] and actual['stats']['route_steps'] > 0
                    print(f'PASS search seed={seed} evaluations<={limit} / route execution and independent scoring', flush=True)
                    continue
                for field in ('status', 'termination', 'makespan', 'source', 'active_cores', 'data_movement_bytes'):
                    assert expected.get(field) == actual.get(field), (field, expected.get(field), actual.get(field))
                def without_time(value):
                    if isinstance(value, dict):
                        return {k: without_time(v) for k, v in value.items() if not k.endswith('seconds')}
                    if isinstance(value, list):
                        return [without_time(v) for v in value]
                    return value
                for field in ('history', 'stats'):
                    error = difference(normalized(without_time(expected[field])), without_time(actual[field]))
                    assert not error, (field, error, directory)
                expected_plan = json.loads((directory/'python/best_checkpoint.json').read_text())['plan']
                actual_plan = json.loads((directory/'cpp/best_checkpoint.json').read_text())['plan']
                assert not difference(expected_plan, actual_plan)
                print(f'PASS search seed={seed} evaluations<={limit} / history, stats, best plan', flush=True)
    finally:
        search.signal = original_signal


if __name__ == '__main__':
    main()
