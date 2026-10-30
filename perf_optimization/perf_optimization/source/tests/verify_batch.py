"""用仅补齐硬件表的原 Python 场景 B 评估器，独立复核批量生成的最终方案。"""
import argparse
from compare_original import *


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('report', type=Path)
    parser.add_argument('--inputs', type=Path, default=ROOT/'hackathon_data')
    parser.add_argument('--work-dir', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text(encoding='utf-8'))
    assert report['status'] == 'complete'
    assert report['completed'] == report['case_count']
    assert report['total_seconds'] < report['total_time_limit'] <= 300
    rows = []
    for index, case in enumerate(report['cases']):
        graph = json.loads((args.inputs/case['input']).read_text(encoding='utf-8'))
        # 参考输入的占位值不参与场景 B 的实际 COPY 计时，只用于通过旧版校验。
        for op in graph['ops']:
            if op['cycles'] == -1 and op['op'] in ('COPY_IN', 'COPY_OUT'):
                op['cycles'] = 1
        plan = json.loads((args.report.parent/case['output']).read_text(encoding='utf-8'))
        expected = evaluate_scene_b(graph, plan, 60, CAPACITY, 1000)
        checkpoint = json.loads((args.work_dir/f'case_{index}/best_checkpoint.json').read_text(encoding='utf-8'))
        assert not difference(plan, checkpoint['plan'])
        error = difference(normalized(expected), checkpoint['official_result'])
        assert not error, (case['input'], error)
        assert case['makespan'] == expected['makespan']
        assert case['data_movement_bytes'] == expected['data_movement_bytes']
        rows.append(dict(input=case['input'], makespan=expected['makespan'], full_result_equal=True))
        print('PASS independent scene-B evaluation:', case['input'], flush=True)
    output = args.report.parent/'verification_report.json'
    output.write_text(json.dumps(dict(reference='Original Python algorithms with PIPE_FIX hardware extension',
        capacity=CAPACITY, bandwidth=60, cross_core_copy_delay=1000, cases=rows), indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
