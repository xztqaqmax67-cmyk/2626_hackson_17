"""Verified embedding of a smaller-core scene-B plan into more available cores.

Only append idle cores: preserve every original subgraph ID, mapping and local
order. The caller must re-evaluate with the unchanged official evaluator before
using this plan as its incumbent. No previous objective is accepted on trust.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

from .utils import sha256
from .core import Plan

OFFICIAL_DIR = Path(__file__).resolve().parent / "official_code"


def instance_fingerprint(graph_path, config_path):
    return {
        'graph_sha256': sha256(Path(graph_path)),
        'config_sha256': sha256(Path(config_path)),
        'official_code': {p.name: sha256(p) for p in sorted((OFFICIAL_DIR).glob('*.py'))},
    }


def lift_plan(graph, plan, ncores):
    """Embed without changing routes, FIFO orders or active core IDs."""
    if type(ncores) is not int or not plan.ncores <= ncores <= 5:
        raise ValueError('lifting requires source cores <= target cores <= 5')
    plan.validate(graph)
    lifted = plan.clone()
    lifted.ncores = ncores
    lifted.validate(graph)
    before, after = plan.to_dict(), lifted.to_dict()
    if (before['node_to_subgraph'] != after['node_to_subgraph']
            or before['core_schedules'] != after['core_schedules'][:plan.ncores]
            or any(after['core_schedules'][plan.ncores:])):
        raise AssertionError('lifting must only append idle cores')
    return lifted


def load_inherited(graph, path, ncores, instance, *, allow_same_core=False):
    path = Path(path).resolve()
    payload = json.loads(path.read_text())
    if payload.get('problem_instance') != instance:
        raise ValueError('inherited checkpoint input/config/official fingerprint mismatch or missing')
    result = payload['official_result']
    value = payload['makespan']
    source_cores = len(payload['plan']['core_schedules'])
    if (result.get('scene') != 'B' or result.get('num_cores') != source_cores
            or not 1 <= source_cores <= ncores
            or (source_cores == ncores and not allow_same_core)
            or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0
            or result.get('makespan') != value):
        raise ValueError('inconsistent or non-smaller-core inherited checkpoint')
    plan = Plan.from_dict(graph, payload['plan'])
    metadata = {
        'status': 'pending', 'checkpoint': Path(path).name, 'checkpoint_sha256': sha256(path),
        'source_cores': source_cores, 'source_makespan': value,
        'source_added_copy_bytes': result['data_movement_bytes']['added_copy_bytes'],
        'source_checkpoint_seconds': payload.get('elapsed_seconds'),
    }
    return lift_plan(graph, plan, ncores), metadata
