"""验证链初解已接入原搜索及内存入口，保留原预算和独立评分。"""
import tempfile

from compare_multicore import branch_fixture
from compare_original import *
from problem2 import official


def invoke(request):
    run = subprocess.run([str(RUNNER)], input=json.dumps(request), text=True,
                         encoding='utf-8', capture_output=True, timeout=60)
    assert run.returncode == 0, run.stdout + run.stderr
    return json.loads(run.stdout.splitlines()[-1])


def main():
    config = ROOT / '前辈代码_cpp/problem2/config.txt'
    settings = official.load_settings(config)
    for seed, cores in ((12, 2), (17, 5)):
        graph = branch_fixture(seed, 40)
        seeds = invoke(dict(action='chain_seeds', graph=graph, capacity=CAPACITY,
                            bandwidth=60, delay=1000, cores=cores, limit=8))['seeds']
        assert seeds and all(row['label'].startswith('chain_') for row in seeds)
        with tempfile.TemporaryDirectory(prefix='chain_integration_', dir=BUILD) as tmp:
            directory = Path(tmp)
            graph_path = directory / 'graph.json'
            graph_path.write_text(json.dumps(graph), encoding='utf-8')
            base = dict(action='solve', graph=graph, capacity=CAPACITY,
                        graph_path=str(graph_path), config_path=str(config), cores=cores,
                        time_limit=30, max_evaluations=24, ablation='full')
            record = invoke(dict(base, output=str(directory/'chain')))
            assert record['solution_available'] and record['status'] == 'complete'
            chain_rows = [row for row in record['history'] if row['source'].startswith('chain_')]
            assert chain_rows and all(row['route'] == 'construction' for row in chain_rows)
            assert record['stats']['full_evaluations'] <= 24
            checkpoint = json.loads((directory/'chain/best_checkpoint.json').read_text(encoding='utf-8'))
            verified = official.evaluate(graph, checkpoint['plan'], settings)
            assert verified['status'] == 'ok', verified
            assert not difference(normalized(verified['official_result']), checkpoint['official_result'])
            assert record['makespan'] == min(row['makespan'] for row in record['history']
                                              if row['status'] == 'ok')
            limited = invoke(dict(base, max_evaluations=1, output=str(directory/'limited')))
            assert limited['stats']['full_evaluations'] == 1 and limited['solution_available']
            memory_plan = invoke(dict(action='solve_memory', graph=graph, capacity=CAPACITY,
                                      cores=cores, time_limit=30, max_evaluations=24))
            assert memory_plan == checkpoint['plan']
            print(f'PASS chain integration seed={seed} cores={cores}: mixed source, budget, '
                  'memory entry, independent Python evaluation', flush=True)


if __name__ == '__main__':
    main()
