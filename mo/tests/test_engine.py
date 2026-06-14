from mo.events import ZeusEvent
from mo.rules import Spec, TriggerRule, AssertRule
from mo.engine import run


def _kinds(result):
    return [v.kind for v in result.verdicts]


def test_matching_event_within_window_fulfills_obligation():
    # OBSERVE tool_declared -> EXPECT tool_called for same id within 5000ms
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_declared", "search", ts=0.0),
        ZeusEvent("tool_called", "search", ts=1000.0),
    ]

    result = run(stream, spec)

    assert _kinds(result) == ["FULFILLED"]
    v = result.verdicts[0]
    assert v.identifier == "search"
    assert v.opened_at == 0.0
    assert v.closed_at == 1000.0
    assert v.spec_line == 1


def test_window_expiry_bolts_on_later_event():
    # tool_called arrives, but past the 5000ms window -> BOLT, no fulfill
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_declared", "search", ts=0.0),
        ZeusEvent("tool_called", "search", ts=6000.0),   # too late
    ]

    result = run(stream, spec)

    assert _kinds(result) == ["BOLTED"]
    v = result.verdicts[0]
    assert v.identifier == "search"
    assert v.closed_at == 6000.0
    assert v.detail["reason"] == "liveness_window_expired"
    assert v.detail["window_ms"] == 5000


def test_open_obligation_at_end_of_stream_bolts():
    # declared, stream ends with nothing fulfilling it and no expiring event
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [ZeusEvent("tool_declared", "search", ts=0.0)]

    result = run(stream, spec)

    assert _kinds(result) == ["BOLTED"]
    assert result.verdicts[0].detail["reason"] == "session_ended_unresolved"


def test_assert_violation_condemns():
    # CONDEMNED: a tool_called whose id was never declared
    def undeclared_call(ev, seen):
        return (ev.primitive == "tool_called"
                and ev.identifier not in seen.get("tool_declared", set()))

    spec = Spec(assertions=[
        AssertRule(spec_line=3, predicate=undeclared_call,
                   detail={"rule": "no undeclared tool_called"}),
    ])
    stream = [ZeusEvent("tool_called", "rm_rf", ts=10.0)]

    result = run(stream, spec)

    assert _kinds(result) == ["CONDEMNED"]
    v = result.verdicts[0]
    assert v.identifier == "rm_rf"
    assert v.spec_line == 3
    assert v.closed_at == 10.0


def test_replay_is_deterministic():
    # same trace -> identical verdicts (no wall-clock reads in the engine)
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_declared", "a", ts=0.0),
        ZeusEvent("tool_declared", "b", ts=100.0),
        ZeusEvent("tool_called", "a", ts=200.0),
        ZeusEvent("tool_called", "b", ts=9000.0),   # b bolts
    ]

    r1 = run(list(stream), spec)
    r2 = run(list(stream), spec)

    assert r1 == r2
    assert sorted(_kinds(r1)) == ["BOLTED", "FULFILLED"]


def test_unmatched_events_counted_for_coverage():
    # premortem #5: events matching no rule are a blindness signal
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_declared", "search", ts=0.0),   # triggers -> matched
        ZeusEvent("log", None, ts=10.0),                # no rule -> unmatched
        ZeusEvent("tool_called", "search", ts=20.0),    # fulfills -> matched
    ]

    result = run(stream, spec)

    assert result.total_events == 3
    assert result.unmatched_events == 1
