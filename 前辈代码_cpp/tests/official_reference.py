"""原封不动的 hackathon.zip 官方参考；不修改 Pipe、输入或官方函数。"""
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys
import hashlib

ROOT = Path(__file__).resolve().parents[2]
OFFICIAL_ROOT = ROOT / '前辈代码_cpp/official_evaluator'
sys.path.insert(0, str(OFFICIAL_ROOT / 'code'))
# 先加载所有官方模块，防止旧 Python 搜索适配器更改 sys.path 后混用旧模块。
for source in sorted((OFFICIAL_ROOT / 'code').glob('*.py')):
    importlib.import_module(source.stem)
from schedule_step1 import step1_schedule
from schedule_step2 import step2_spill_insertion, _build_extended_graph
from schedule_step3 import step3_simulation
from stub_multicore_cut_and_schedule import generate_multicore_plan
from multicore_cut_evaluate_problem_2 import evaluate_scene_b
from evaluation_validation import validate_graph, EvaluationValidationError
BUILD = Path(os.environ.get('PROBLEM2_BUILD', ROOT / '前辈代码_cpp/build'))
RUNNER = Path(os.environ.get('PROBLEM2_RUNNER', BUILD / 'bin' / ('parity_runner.exe' if sys.platform == 'win32' else 'parity_runner')))
CAPACITY = {'L1': 1572864, 'UB': 131072}

def normalized(value):
    return json.loads(json.dumps(value, default=lambda x: sorted(x) if isinstance(x, set) else x))

def difference(a, b, path='$'):
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys():
            return f'{path}: keys {a.keys() ^ b.keys()}'
        for key in a:
            diff = difference(a[key], b[key], f'{path}.{key}')
            if diff:
                return diff
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f'{path}: lengths {len(a)} != {len(b)}'
        for index, (x, y) in enumerate(zip(a, b)):
            diff = difference(x, y, f'{path}[{index}]')
            if diff:
                return diff
    elif a != b:
        return f'{path}: {a!r} != {b!r}'

def verify_official_sources():
    manifest = json.loads((OFFICIAL_ROOT / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    for name, digest in manifest['files'].items():
        if hashlib.sha256((OFFICIAL_ROOT / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError(f'official source changed: {name}')
    for source in (OFFICIAL_ROOT / 'code').glob('*.py'):
        if Path(sys.modules[source.stem].__file__).resolve() != source.resolve():
            raise RuntimeError(f'wrong official module loaded: {source.stem}')
    return manifest


def run_cpp(request, runner=RUNNER, timeout=300):
    run = subprocess.run([str(runner)], input=json.dumps(request), text=True,
                         encoding='utf-8', capture_output=True, timeout=timeout)
    result = json.loads(run.stdout)
    if run.returncode or 'error' in result:
        raise RuntimeError(result.get('error', run.stderr))
    return result

verify_official_sources()
