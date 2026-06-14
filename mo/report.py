"""Render a RunResult: machine JSONL (one verdict per line) + human summary.

FULFILLED is logged, not just deviations — it buys coverage stats for free
(open question §8). The summary always prints BOLTED/CONDEMNED counts even at
zero so a clean session reads as an explicit pass, not silence.
"""
from __future__ import annotations

from dataclasses import asdict

from .engine import RunResult, Verdict

KINDS = ("FULFILLED", "BOLTED", "CONDEMNED")


def verdict_to_dict(v: Verdict) -> dict:
    return asdict(v)


def jsonl_lines(result: RunResult):
    import json
    for v in result.verdicts:
        yield json.dumps(verdict_to_dict(v))


def human_summary(result: RunResult) -> str:
    counts = {k: 0 for k in KINDS}
    for v in result.verdicts:
        counts[v.kind] = counts.get(v.kind, 0) + 1

    lines = ["MO verdict summary", "=================="]
    for k in KINDS:
        lines.append(f"  {k}: {counts[k]}")
    lines.append(
        f"  coverage: {result.unmatched_events}/{result.total_events} "
        f"events matched no rule"
    )
    return "\n".join(lines)
