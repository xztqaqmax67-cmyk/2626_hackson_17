"""备注约束：五条独立 Pipe、COPY 占位、配置、原生批量预算和失败不冒充成功。"""
import tempfile
import time
from compare_original import *
from compare_workflow import BIN, SUFFIX, run


def copy_fixture():
    return dict(ops=[
        dict(id=1, op='COPY_IN', pipe='PIPE_MTE2', cycles=-1),
        dict(id=2, op='L1_COPY_UB', pipe='PIPE_FIX', cycles=68),
        dict(id=3, op='COMPUTE', pipe='PIPE_V', cycles=10),
        dict(id=4, op='COPY_OUT', pipe='PIPE_MTE3', cycles=-1)],
        tensors=[dict(id=100, pos='DDR', size=1048576), dict(id=101, pos='L1', size=1048576),
                 dict(id=102, pos='UB', size=32768), dict(id=103, pos='UB', size=32768),
                 dict(id=104, pos='DDR', size=32768)],
        edges=[dict(source=a, target=b) for a,b in
               [(100,1),(1,101),(101,2),(2,102),(102,3),(3,103),(103,4),(4,104)]])


def main():
    graph = copy_fixture()
    reference = normalized(graph)
    for op in reference['ops']:
        if op['cycles'] == -1:
            op['cycles'] = 1
    plan = generate_multicore_plan(reference, num_cores=3)
    compare('generate', graph, plan, cores=3)
    compare('scene_b', graph, evaluate_scene_b(reference, plan, 60, CAPACITY, 1000), plan=plan, delay=1000)
    parallel = dict(tensors=[], edges=[], ops=[
        dict(id=1, op='M', pipe='PIPE_M', cycles=20),
        dict(id=2, op='FIX', pipe='PIPE_FIX', cycles=10),
        dict(id=3, op='V', pipe='PIPE_V', cycles=5),
        dict(id=4, op='FIX', pipe='PIPE_FIX', cycles=4)])
    sequence = step1_schedule(parallel)
    extended = _extended(parallel, sequence)
    expected = step3_simulation(extended, CAPACITY, 60)
    assert expected['makespan'] == 20, 'FIX 应与 M/V 独立重叠执行，同 FIX 操作保持串行'
    compare('step3', extended, expected)
    for kind in ('negative_compute', 'invalid_copy', 'unknown_pipe'):
        invalid = normalized(graph)
        if kind == 'negative_compute':
            invalid['ops'][1]['cycles'] = -1
        elif kind == 'invalid_copy':
            invalid['ops'][0]['cycles'] = -2
        else:
            invalid['ops'][1]['pipe'] = 'PIPE_UNKNOWN'
        compare_rejection('generate', invalid)

    directory = Path(tempfile.mkdtemp(prefix='hackathon_contract_', dir=BUILD))
    inputs, output, work = directory/'任意图名', directory/'输出', directory/'工作'
    inputs.mkdir()
    for name in ('matmul_a', 'conv_b'):
        (inputs/f'{name}.json').write_text(json.dumps(graph), encoding='utf-8')
    config = ROOT/'前辈代码_cpp/problem2/config.txt'
    started = time.monotonic()
    result = run(BIN/'problem2.exe', [inputs, '-n', 3, '--config', config, '--output-dir', output,
        '--work-dir', work, '--total-time-limit', 8])
    batch = json.loads(result.stdout)
    assert batch['status'] == 'complete' and batch['completed'] == 2
    assert batch['total_seconds'] < 8 and time.monotonic()-started < 8
    for target in output.glob('*_multicore_res.json'):
        value = json.loads(target.read_text(encoding='utf-8'))
        assert set(value) == {'node_to_subgraph', 'core_schedules'}
        assert set(value['node_to_subgraph']) == {'2', '3'}
    single = json.loads(run(BIN/'problem2.exe', [inputs/'conv_b.json', '-n', 3, '--config', config,
        '-o', directory/'单例.json']).stdout)
    assert single['time_limit'] == 25 and single['feasibility_grace_seconds'] == 0

    invalid_inputs, old_output = directory/'错误图', directory/'旧输出'
    invalid_inputs.mkdir()
    old_output.mkdir()
    (invalid_inputs/'broken.json').write_text(json.dumps(invalid), encoding='utf-8')
    old_plan = old_output/'broken_multicore_res.json'
    old_plan.write_text('{"old":true}', encoding='utf-8')
    failed = subprocess.run([str(BIN/('problem2'+SUFFIX)), str(invalid_inputs), '-n', '3',
        '--config', str(config), '--output-dir', str(old_output), '--total-time-limit', '5'],
        encoding='utf-8', text=True, capture_output=True, timeout=7)
    assert failed.returncode != 0
    failure_report = json.loads(failed.stdout)
    assert failure_report['completed'] == 0 and not failure_report['cases'][0]['solution_available']
    assert old_plan.read_text(encoding='utf-8') == '{"old":true}'
    print('PASS Hackathon: config, independent PIPE_FIX, COPY placeholders, defaults, batch and stale output', flush=True)


def _extended(graph, sequence):
    # 下划线函数不会被 import * 引入。
    from schedule_step2 import _build_extended_graph
    return _build_extended_graph(graph, step2_spill_insertion(graph, sequence, CAPACITY))


if __name__ == '__main__':
    main()
