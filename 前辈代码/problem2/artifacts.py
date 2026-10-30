"""Appendix B evaluation files from an already officially evaluated result."""
from pathlib import Path
import sys
import math
from .utils import atomic_write, read_json


def export_evaluation(directory, input_name, plan_name, result):
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'official_code'))
    from contest_io import format_scene_b_trace_json, format_multicore_result_log
    directory = Path(directory)
    stem = Path(input_name).stem
    trace = format_scene_b_trace_json(input_name, result)
    standard = dict(result, input_graph=input_name, input_plan=plan_name)
    atomic_write(directory / f'{stem}_problem_2_res.json', standard)
    atomic_write(directory / f'{stem}_problem_2_trace.json', trace + '\n')
    atomic_write(directory / f'{stem}_problem_2_log.txt', format_multicore_result_log(input_name, standard))
    return trace


def recover(directory, case, cores):
    payload = read_json(directory / 'best_checkpoint.json')
    if not payload:
        return None
    result, plan = payload['official_result'], payload['plan']
    if (result.get('scene') != 'B' or not math.isfinite(result['makespan']) or result['makespan'] < 0
            or result['num_cores'] != cores or len(plan['core_schedules']) != cores
            or result['makespan'] != payload['makespan']):
        raise ValueError('inconsistent checkpoint')
    case_name = f'case_{case:03d}' if isinstance(case, int) else str(case)
    atomic_write(directory / 'plan.json', plan)
    atomic_write(directory / f'{case_name}_multicore_res.json', plan)
    atomic_write(directory / 'official_result.json', result)
    trace = export_evaluation(directory, f'{case_name}.json', f'{case_name}_multicore_res.json', result)
    atomic_write(directory / 'trace.json', trace)
    return payload

