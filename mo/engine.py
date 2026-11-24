"""The verdict engine.

A `Judge` holds the obligation ledger and evaluates one event at a time with
`observe()`. `observe()` never finalises: it does not emit end-of-stream BOLTs
and it does not touch the wall clock. `finish()` is the only finaliser.

That split is what makes a *pre-delivery* gate possible: a caller can judge an
intended effect on a forked judge, decide, and commit the state only if the
effect is actually allowed to happen. See `mo/gate.py`.
"""
from __future__ import annotations

from collections import defaultdict
from dataclasses import dataclass, field, fields

from .events import ZeusEvent
from .ledger import Obligation, ObligationLedger
from .rules import AssertRule, Spec


@dataclass
class Verdict:
    kind: str                    # FULFILLED | BOLTED | CONDEMNED
    identifier: str | None
    spec_line: int
    opened_at: float | None
    closed_at: float
    detail: dict = field(default_factory=dict)
    spec_source: str | None = None   # fragment file the rule came from


@dataclass
class RunResult:
    verdicts: list[Verdict] = field(default_factory=list)
    total_events: int = 0
    unmatched_events: int = 0    # coverage signal (premortem #5)


@dataclass
class Observation:
    verdicts: list[Verdict] = field(default_factory=list)
    matched: bool = False        # did any rule claim this event?
    pending_assertion: bool = False  # a deferred ASSERT is inside its grace window


@dataclass
class PendingAssert:
    rule: AssertRule
    event: ZeusEvent
    deadline: float


def _bolt(ob: Obligation, closed_at: float, reason: str) -> Verdict:
    detail = {"window_ms": ob.expires_at - ob.opened_at, "reason": reason}
    # `closed_at` is notice-time: the event ts that revealed the lapse, which
    # can arrive long after the window actually expired. The lapse itself is
    # the window boundary, so record it (and the reporting lag) exactly rather
    # than letting a late tick/flush make a 5s window look like a longer span.
    if closed_at > ob.expires_at:
        detail["lapsed_at"] = ob.expires_at
        detail["notice_lag_ms"] = closed_at - ob.expires_at
    if ob.correlation is not None:
        detail["correlation"] = ob.correlation
    return Verdict(
        kind="BOLTED",
        identifier=ob.identifier,
        spec_line=ob.spec_line,
        opened_at=ob.opened_at,
        closed_at=closed_at,
        detail=detail,
        spec_source=ob.spec_source,
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
        spec_source=ob.spec_source,
    )


def _condemn(ev, rule) -> Verdict:
    return Verdict(
        kind="CONDEMNED",
        identifier=ev.identifier,
        spec_line=rule.spec_line,
        opened_at=None,
        closed_at=ev.ts,
        detail={"event": ev.primitive, **rule.detail},
        spec_source=rule.spec_source,
    )


def _condemn_pending(p: PendingAssert, closed_at: float, reason: str) -> Verdict:
    ev = p.event
    return Verdict(
        kind="CONDEMNED",
        identifier=ev.identifier,
        spec_line=p.rule.spec_line,
        opened_at=None,
        closed_at=closed_at,
        detail={
            "event": ev.primitive,
            **p.rule.detail,
            "call_ts": ev.ts,
            "grace_ms": p.rule.grace_ms,
            "reason": reason,
        },
        spec_source=p.rule.spec_source,
    )


class Judge:
    """Stateful evaluator. Deterministic: time only advances via event ts."""

    def __init__(self, spec: Spec) -> None:
        self.spec = spec
        self.ledger = ObligationLedger()
        self.seen: dict = defaultdict(set)
        self.pending_asserts: list[PendingAssert] = []
        self.last_ts = 0.0
        self.total_events = 0
        self.unmatched_events = 0

    def fork(self) -> "Judge":
        """An independent copy of the current state.

        Used to judge an intended effect without committing it: if the effect
        is denied, the original judge never learns that it happened.
        """
        clone = Judge(self.spec)
        clone.ledger._obligations = [
            Obligation(**{f.name: getattr(ob, f.name) for f in fields(ob)})
            for ob in self.ledger._obligations
        ]
        clone.seen = defaultdict(set, {k: set(v) for k, v in self.seen.items()})
        clone.pending_asserts = [
            PendingAssert(p.rule, p.event, p.deadline)
            for p in self.pending_asserts
        ]
        clone.last_ts = self.last_ts
        clone.total_events = self.total_events
        clone.unmatched_events = self.unmatched_events
        return clone

    def observe(self, ev) -> Observation:
        """Judge one event. No end-of-stream BOLT, no wall clock."""
        verdicts: list[Verdict] = []
        pending_assertion = False
        self.last_ts = ev.ts

        # a tick is a pure time carrier (premortem #1, the silence problem):
        # it advances the clock and expires windows, but never fulfills,
        # condemns, triggers, or counts toward coverage (premortem #5).
        if ev.primitive == "tick":
            for ob in self.ledger.open_obligations():
                if ev.ts > ob.expires_at:
                    self.ledger.close(ob, "bolted")
                    verdicts.append(_bolt(ob, ev.ts, "liveness_window_expired"))
            verdicts.extend(self._expire_pending(ev.ts))
            return Observation(verdicts, True)

        self.total_events += 1
        self.seen[ev.primitive].add(ev.identifier)
        matched = False

        # a) expire first, against THIS event's timestamp (deterministic)
        for ob in self.ledger.open_obligations():
            if ev.ts > ob.expires_at:
                self.ledger.close(ob, "bolted")
                verdicts.append(_bolt(ob, ev.ts, "liveness_window_expired"))
        verdicts.extend(self._expire_pending(ev.ts))

        # b) a declaration resolves any pending wire-order ASSERT for its id
        self._resolve_pending(ev)

        # c) satisfy any open obligation this event closes
        for ob in self.ledger.open_obligations():
            if ev.primitive == ob.expects and ev.identifier == ob.identifier:
                # correlation-aware matching (premortem #2): only an
                # obligation opened by an instance event is correlation-scoped.
                # A declaration carries no instance id, so its obligation must
                # accept the first matching execution even though that event
                # has one; requiring the absent side to agree would make the
                # canonical declaration->execution spec BOLT every honored tool.
                if ob.correlation is not None and ev.correlation != ob.correlation:
                    continue
                self.ledger.close(ob, "fulfilled")
                verdicts.append(_fulfill(ob, ev.ts))
                matched = True
                break

        # d) safety: does this event violate an ASSERT?
        for rule in self.spec.assertions:
            if rule.violated_by(ev, self.seen):
                if rule.grace_ms is None:
                    verdicts.append(_condemn(ev, rule))
                else:
                    # The declaration may still be in flight on the wire. Hold
                    # the verdict until the grace deadline, where a tick or a
                    # later event either resolves it or condemns it.
                    self.pending_asserts.append(
                        PendingAssert(rule, ev, ev.ts + rule.grace_ms)
                    )
                    pending_assertion = True
                matched = True

        # e) does this event TRIGGER new obligations?
        for rule in self.spec.triggers:
            if rule.fires_on(ev):
                self.ledger.open(rule.make_obligation(ev))
                matched = True

        if not matched:
            self.unmatched_events += 1

        return Observation(verdicts, matched, pending_assertion)

    def _expire_pending(self, at_ts: float) -> list[Verdict]:
        """Condemn deferred ASSERTs whose grace window this event has crossed."""
        expired: list[PendingAssert] = []
        still_pending: list[PendingAssert] = []
        for p in self.pending_asserts:
            (expired if at_ts > p.deadline else still_pending).append(p)
        self.pending_asserts = still_pending
        return [
            _condemn_pending(p, at_ts, "late_declaration_grace_expired")
            for p in expired
        ]

    def _resolve_pending(self, ev: ZeusEvent) -> None:
        """A declaration closes pending checks for the same identifier."""
        if not self.pending_asserts:
            return
        self.pending_asserts = [
            p for p in self.pending_asserts
            if not (p.rule.primitive_b == ev.primitive
                    and p.event.identifier == ev.identifier)
        ]

    def finish(self) -> list[Verdict]:
        """End of stream: open obligations BOLT; pending ASSERTs condemn."""
        verdicts: list[Verdict] = []
        for ob in self.ledger.open_obligations():
            self.ledger.close(ob, "bolted")
            verdicts.append(_bolt(ob, self.last_ts, "session_ended_unresolved"))
        for p in self.pending_asserts:
            verdicts.append(
                _condemn_pending(p, self.last_ts, "session_ended_undeclared")
            )
        self.pending_asserts = []
        return verdicts


def iter_verdicts(stream, spec: Spec, stats: RunResult | None = None):
    """The judge as a generator: yield each Verdict the moment it is decided.

    `run()` drains it into a RunResult for batch/replay; `mo watch` consumes it
    lazily so a BOLT prints the instant a window lapses rather than at session
    end. Coverage counters can't ride along in the verdict yield, so a caller
    wanting them passes a RunResult as `stats` and the loop fills it in at the
    end.
    """
    judge = Judge(spec)

    for ev in stream:
        for v in judge.observe(ev).verdicts:
            yield v

    for v in judge.finish():
        yield v

    if stats is not None:
        stats.total_events = judge.total_events
        stats.unmatched_events = judge.unmatched_events


def run(stream, spec: Spec) -> RunResult:
    result = RunResult()
    result.verdicts = list(iter_verdicts(stream, spec, stats=result))
    return result
