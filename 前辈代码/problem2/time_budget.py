"""Only a missing first complete checkpoint permits a bounded time extension."""
from pathlib import Path
import subprocess
import time


def wait_for_search(child, target_deadline, hard_deadline, checkpoint):
    """Return (returncode, must_kill, extension_used); never kill unrelated work."""
    try:
        return child.wait(timeout=max(0., target_deadline - time.monotonic())), False, False
    except subprocess.TimeoutExpired:
        pass
    checkpoint = Path(checkpoint)
    if checkpoint.exists() or hard_deadline <= time.monotonic():
        return None, True, False
    while time.monotonic() < hard_deadline:
        try:
            return child.wait(timeout=min(.2, max(0., hard_deadline - time.monotonic()))), False, True
        except subprocess.TimeoutExpired:
            if checkpoint.exists():
                # Allow atomic metadata/finalization, never another search round.
                try:
                    return child.wait(timeout=min(.5, max(0., hard_deadline - time.monotonic()))), False, True
                except subprocess.TimeoutExpired:
                    return None, True, True
    return None, True, True


def core_targets(cores, case_budget=600., time_limit=None):
    """Selected configurations share one case target; default 2..5 = 60/120/180/240."""
    if time_limit is not None:
        return {n: float(time_limit) for n in cores}
    weights = {n: max(1, n - 1) for n in cores}
    total = sum(weights.values())
    return {n: case_budget * weights[n] / total for n in cores}


def case_slot_limits(slot_start, now, target, case_deadline, future_budget, grace_limit):
    """Reserve later targets and final export time before granting feasibility grace."""
    reserve = min(3., max(.1, target * .05))
    allowance = min(slot_start + target, case_deadline - future_budget) - now - reserve
    grace = max(0., min(grace_limit,
                       case_deadline - future_budget - now - allowance - reserve))
    return allowance, grace, reserve
