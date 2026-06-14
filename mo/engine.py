"""The verdict engine. No cursor: each event expires, satisfies, condemns,
and triggers against the ledger. Deterministic on replay because time only
advances via event timestamps (premortem #6 — never read wall clock here).
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field

from .ledger import Obligation, ObligationLedger
from .rules import Spec


@dataclass
class Verdict:
    kind: str                    # FULFILLED | BOLTED | CONDEMNED
    identifier: str | None
    spec_line: int
    opened_at: float | None
    closed_at: float
    detail: dict = field(default_factory=dict)


@dataclass
class RunResult:
    verdicts: list[Verdict] = field(default_factory=list)
    total_events: int = 0
    unmatched_events: int = 0    # coverage signal (premortem #5)


def _bolt(ob: Obligation, closed_at: float, reason: str) -> Verdict:
    return Verdict(
        kind="BOLTED",
        identifier=ob.identifier,
        spec_line=ob.spec_line,
        opened_at=ob.opened_at,
        closed_at=closed_at,
        detail={"window_ms": ob.expires_at - ob.opened_at, "reason": reason},
    )


def _fulfill(ob: Obligation, closed_at: float) -> Verdict:
    return Verdict(
        kind="FULFILLED",
        identifier=ob.identifier,
        spec_line=ob.spec_line,
        opened_at=ob.opened_at,
        closed_at=closed_at,
        detail={},
    )


def _condemn(ev, rule) -> Verdict:
    return Verdict(
        kind="CONDEMNED",
        identifier=ev.identifier,
        spec_line=rule.spec_line,
        opened_at=None,
        closed_at=ev.ts,
        detail={"event": ev.primitive, **rule.detail},
    )


def run(stream, spec: Spec) -> RunResult:
    ledger = ObligationLedger()
    result = RunResult()
    seen: dict = defaultdict(set)   # primitive -> set of identifiers observed
    last_ts = 0.0

    for ev in stream:
        result.total_events += 1
        last_ts = ev.ts
        seen[ev.primitive].add(ev.identifier)
        matched = False

        # a) expire first, against THIS event's timestamp (deterministic)
        for ob in ledger.open_obligations():
            if ev.ts > ob.expires_at:
                result.verdicts.append(_bolt(ob, ev.ts, "liveness_window_expired"))
                ledger.close(ob, "bolted")

        # b) satisfy any open obligation this event closes
        for ob in ledger.open_obligations():
            if ev.primitive == ob.expects and ev.identifier == ob.identifier:
                result.verdicts.append(_fulfill(ob, ev.ts))
                ledger.close(ob, "fulfilled")
                matched = True

        # c) safety: does this event violate an ASSERT?
        for rule in spec.assertions:
            if rule.violated_by(ev, seen):
                result.verdicts.append(_condemn(ev, rule))
                matched = True

        # d) does this event TRIGGER new obligations?
        for rule in spec.triggers:
            if rule.fires_on(ev):
                ledger.open(rule.make_obligation(ev))
                matched = True

        if not matched:
            result.unmatched_events += 1

    # e) end of stream: every still-open obligation is a BOLT
    for ob in ledger.open_obligations():
        result.verdicts.append(_bolt(ob, last_ts, "session_ended_unresolved"))
        ledger.close(ob, "bolted")

    return result
