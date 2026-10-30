"""新官方评分的关键语义回归，不依赖旧 Python 求解器。"""
from official_reference import *
from stub_multicore_cut_and_schedule import derive_multicore_plan, MulticoreCutError
from schedule_step2 import Step2SchedulingError


def spill_fixture(dirty=True, original_backing=False):
    ops = [dict(id=i, op='COMPUTE', pipe='PIPE_V', cycles=10) for i in range(1, 8)]
    tensors = [dict(id=100, pos='UB', size=40), dict(id=101, pos='UB', size=55),
               dict(id=102, pos='UB', size=55)]
    pairs = [(1, 100), (4, 100) if dirty else (100, 4), (100, 7),
             (2, 101), (101, 3), (5, 102), (102, 6)]
    pairs += [(i, i+1) for i in range(1, 7)]
    if original_backing:
        ops[0].update(op='COPY_IN', pipe='PIPE_MTE2', cycles=-1)
        tensors.append(dict(id=103, pos='DDR', size=40))
        pairs.append((103, 1))
    return dict(ops=ops, tensors=tensors, edges=[dict(source=a, target=b) for a, b in pairs])


def compare_step2(graph, sequence, capacity):
    expected = step2_spill_insertion(graph, sequence, capacity)
    actual = run_cpp(dict(action='step2', graph=graph, sequence=sequence, capacity=capacity))
    assert not difference(normalized(expected), actual), difference(normalized(expected), actual)
    extended = _extended(graph, expected)
    result = step3_simulation(extended, capacity, 60)
    actual3 = run_cpp(dict(action='step3', graph=extended, capacity=capacity, bandwidth=60))
    assert not difference(normalized(result), actual3), difference(normalized(result), actual3)
    return expected


def _extended(graph, result):
    from schedule_step2 import _build_extended_graph
    return _build_extended_graph(graph, result)


def main():
    verify_official_sources()
    for dirty in (False, True):
        for backing in (False, True):
            graph = spill_fixture(dirty, backing)
            result = compare_step2(graph, list(range(1, 8)), {'L1': 100, 'UB': 100})
            records = result['spill_records']
            assert len(records) == 2
            assert [r['spill_out_copies_data'] for r in records] == [not backing, dirty]
            if dirty:
                assert dict(source=4, target=records[0]['to_tid']) in result['ext_edges']
            print(f'PASS spill, rename, dirty writeback: dirty={dirty}, original_backing={backing}', flush=True)
    for size in (90, 95, 100):
        graph = dict(ops=[dict(id=1, op='COMPUTE', pipe='PIPE_FIX', cycles=5)],
                     tensors=[dict(id=2, pos='UB', size=size)], edges=[dict(source=1, target=2)])
        result = compare_step2(graph, [1], {'UB': 100, 'L1': 100})
        assert not result['spill_records']
        assert bool(result['overflow_log']) == (size > 90)
    graph['tensors'][0]['size'] = 101
    try:
        step2_spill_insertion(graph, [1], {'UB': 100, 'L1': 100})
        raise AssertionError('official accepted hard-capacity overflow')
    except Step2SchedulingError:
        pass
    try:
        run_cpp(dict(action='step2', graph=graph, sequence=[1], capacity={'UB': 100, 'L1': 100}))
        raise AssertionError('C++ accepted hard-capacity overflow')
    except RuntimeError as error:
        assert 'no spill victim' in str(error)
    # 原图有效，但传给 Step2 的顺序错误：输出契约必须拒绝反向直接依赖。
    bad_order = dict(ops=[dict(id=i, op='COMPUTE', pipe='PIPE_V', cycles=1) for i in (1, 2)],
                     tensors=[], edges=[dict(source=1, target=2)])
    for evaluator in (lambda: step2_spill_insertion(bad_order, [2, 1], CAPACITY),
                      lambda: run_cpp(dict(action='step2', graph=bad_order, sequence=[2, 1],
                                           capacity=CAPACITY))):
        try:
            evaluator()
            raise AssertionError('accepted non-topological extended sequence')
        except (Step2SchedulingError, RuntimeError) as error:
            assert 'seq_ext is not topological' in str(error)
    graph = spill_fixture()
    invalid = dict(node_to_subgraph={str(i): i for i in range(1, 8)},
                   core_schedules=[[1, 2, 3], [4, 5, 6, 7]])
    for evaluator in (lambda: derive_multicore_plan(graph, invalid),
                      lambda: run_cpp(dict(action='scene_b', graph=graph, plan=invalid,
                                           capacity=CAPACITY, bandwidth=60, delay=1000))):
        try:
            evaluator()
            raise AssertionError('accepted writers on multiple cores')
        except (MulticoreCutError, RuntimeError) as error:
            assert 'writers on multiple cores' in str(error)
    for cores in (2, 3, 4, 5):
        for seed in (0, 7, 4294967297):
            plan = generate_multicore_plan(graph, cores, seed, 1, 1)
            actual = run_cpp(dict(action='generate', graph=graph, cores=cores, seed=seed,
                                  min_size=1, max_size=1))
            assert normalized(plan) == actual
            expected = evaluate_scene_b(graph, plan, 60, CAPACITY, 1000)
            actual = run_cpp(dict(action='scene_b', graph=graph, plan=plan,
                                  capacity=CAPACITY, bandwidth=60, delay=1000))
            assert not difference(normalized(expected), actual)
    print('PASS hard/soft capacity, writer co-location, generators and full evaluation on 2..5 cores')


if __name__ == '__main__':
    main()
