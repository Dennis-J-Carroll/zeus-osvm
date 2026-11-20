from mo.events import ZeusEvent
from mo.engine import Judge
from mo.gate import ALLOW, DENY, ESCALATE, Gate
from mo.rules import Spec, TriggerRule, AssertRule


def _liveness_spec():
    return Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])


def test_unmatched_effect_escalates():
    gate = Gate(_liveness_spec())
    decision = gate.check(ZeusEvent("log", None, ts=10.0))

    assert decision.action == ESCALATE
    assert decision.reason == "unmatched_effect"


def test_escalation_does_not_open_or_commit_state():
    # Two escalations of the same event must not accumulate obligations; and
    # ending the session afterwards must not bolt something that was refused.
    gate = Gate(_liveness_spec())
    assert gate.check(ZeusEvent("log", None, ts=10.0)).action == ESCALATE
    assert gate.check(ZeusEvent("log", None, ts=20.0)).action == ESCALATE
    assert gate.finish() == []


def test_triggering_event_is_allowed_and_opens_obligation():
    gate = Gate(_liveness_spec())
    decision = gate.check(ZeusEvent("tool_declared", "search", ts=0.0))

    assert decision.action == ALLOW
    assert decision.reason == "matched_rule"

    # The ALLOW committed the trigger, so the unmet obligation bolts at finish.
    bolted = gate.finish()
    assert [v.kind for v in bolted] == ["BOLTED"]
    assert bolted[0].identifier == "search"


def test_fulfilling_event_is_allowed():
    gate = Gate(_liveness_spec())
    gate.check(ZeusEvent("tool_declared", "search", ts=0.0))
    decision = gate.check(ZeusEvent("tool_called", "search", ts=100.0))

    assert decision.action == ALLOW
    assert [v.kind for v in decision.verdicts] == ["FULFILLED"]
    assert gate.finish() == []


def test_assertion_violation_denies():
    def undeclared(ev, seen):
        return (ev.primitive == "tool_called"
                and ev.identifier not in seen.get("tool_declared", set()))

    spec = Spec(assertions=[
        AssertRule(spec_line=3, predicate=undeclared,
                   detail={"rule": "no undeclared tool_called"}),
    ])
    gate = Gate(spec)
    decision = gate.check(ZeusEvent("tool_called", "rm_rf", ts=5.0))

    assert decision.action == DENY
    assert decision.reason == "assertion_violated"
    assert [v.kind for v in decision.verdicts] == ["CONDEMNED"]


def test_denied_effect_never_enters_the_ledger():
    # If the denied event had been committed, the ASSERT's seen set would grow
    # but no obligation exists; instead the real proof is that a subsequent
    # matching event still behaves as if the denied one never happened.
    def always(ev, seen):
        return ev.primitive == "forbidden"

    spec = Spec(
        triggers=[TriggerRule("tool_declared", "tool_called", 5000, 1)],
        assertions=[AssertRule(spec_line=2, predicate=always)],
    )
    gate = Gate(spec)
    assert gate.check(ZeusEvent("forbidden", "x", ts=0.0)).action == DENY

    # The refusal did not touch the ledger: no obligation was opened, so there
    # is nothing to bolt at the end of the session.
    assert gate.finish() == []


def test_observe_is_not_a_finaliser():
    judge = Judge(_liveness_spec())
    judge.observe(ZeusEvent("tool_declared", "search", ts=0.0))

    # observe() alone must not emit the end-of-stream BOLT...
    assert judge.ledger.open_obligations()
    # ...but finish() must.
    verdicts = judge.finish()
    assert [v.kind for v in verdicts] == ["BOLTED"]
    assert verdicts[0].detail["reason"] == "session_ended_unresolved"
