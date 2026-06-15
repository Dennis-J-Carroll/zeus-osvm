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
    detail = {"window_ms": ob.expires_at - ob.opened_at, "reason": reason}
    if ob.correlation is not None:
        detail["correlation"] = ob.correlation
    return Verdict(
        kind="BOLTED",
        identifier=ob.identifier,
        spec_line=ob.spec_line,
        opened_at=ob.opened_at,
        closed_at=closed_at,
        detail=detail,
    )


def _fulfill(ob: Obligation, closed_at: float) -> Verdict:
    detail = {}
    if ob.correlation is not None:
        detail["correlation"] = ob.correlation
    return Verdict(
        kind="FULFILLED",
        identifier=ob.identifier,
        spec_line=ob.spec_line,
        opened_at=ob.opened_at,
        closed_at=closed_at,
        detail=detail,
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


def iter_verdicts(stream, spec: Spec, stats: RunResult | None = None):
    """The judge as a generator: yield each Verdict the moment it is decided.

    This is the one evaluation loop. `run()` drains it into a RunResult for
    batch/replay; `mo watch` consumes it lazily so a live session prints a BOLT
    the instant a window lapses rather than at session end. Coverage counters
    can't ride along in the verdict yield, so a caller wanting them passes a
    RunResult as `stats` and the loop updates it in place.
    """
    ledger = ObligationLedger()
    seen: dict = defaultdict(set)   # primitive -> set of identifiers observed
    last_ts = 0.0

    for ev in stream:
        last_ts = ev.ts

        # a tick is a pure time carrier (premortem #1, the silence problem):
        # it only advances the clock so open windows can expire during silence.
        # It is NOT a wire event — it never fulfills, condemns, or triggers, and
        # it stays out of the coverage denominator (premortem #5) so a quiet
        # live session does not read as "all blind". The wall clock that mints
        # ticks lives in the adapter; the engine still only reads ev.ts (#6).
        if ev.primitive == "tick":
            for ob in ledger.open_obligations():
                if ev.ts > ob.expires_at:
                    ledger.close(ob, "bolted")
                    yield _bolt(ob, ev.ts, "liveness_window_expired")
            continue

        if stats is not None:
            stats.total_events += 1
        seen[ev.primitive].add(ev.identifier)
        matched = False

        # a) expire first, against THIS event's timestamp (deterministic)
        for ob in ledger.open_obligations():
            if ev.ts > ob.expires_at:
                ledger.close(ob, "bolted")
                yield _bolt(ob, ev.ts, "liveness_window_expired")

        # b) satisfy any open obligation this event closes
        for ob in ledger.open_obligations():
            if ev.primitive == ob.expects and ev.identifier == ob.identifier:
                # correlation-aware matching (premortem #2): if either side has
                # an instance id, they must agree exactly. When neither has one,
                # fall back to first-match by identifier (the M0/M1 behavior).
                if (ob.correlation is not None or ev.correlation is not None) \
                        and ev.correlation != ob.correlation:
                    continue
                ledger.close(ob, "fulfilled")
                yield _fulfill(ob, ev.ts)
                matched = True
                break

        # c) safety: does this event violate an ASSERT?
        for rule in spec.assertions:
            if rule.violated_by(ev, seen):
                yield _condemn(ev, rule)
                matched = True

        # d) does this event TRIGGER new obligations?
        for rule in spec.triggers:
            if rule.fires_on(ev):
                ledger.open(rule.make_obligation(ev))
                matched = True

        if not matched and stats is not None:
            stats.unmatched_events += 1

    # e) end of stream: every still-open obligation is a BOLT
    for ob in ledger.open_obligations():
        ledger.close(ob, "bolted")
        yield _bolt(ob, last_ts, "session_ended_unresolved")


def run(stream, spec: Spec) -> RunResult:
    result = RunResult()
    result.verdicts = list(iter_verdicts(stream, spec, stats=result))
    return result
