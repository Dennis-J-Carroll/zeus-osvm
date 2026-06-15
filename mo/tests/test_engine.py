from mo.events import ZeusEvent
from mo.rules import Spec, TriggerRule, AssertRule
from mo.engine import run, iter_verdicts


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


def test_iter_verdicts_yields_before_the_stream_is_drained():
    # live mode needs verdicts the moment they are decided, not at session end.
    # iter_verdicts must be lazy: a verdict comes out before the trailing
    # events are even pulled from the source.
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    pulled = []

    def source():
        pulled.append("call"); yield ZeusEvent("tool_called", "s", ts=0.0)
        pulled.append("result"); yield ZeusEvent("tool_result", "s", ts=10.0)
        pulled.append("trailing"); yield ZeusEvent("log", None, ts=20.0)

    it = iter_verdicts(source(), spec)
    first = next(it)

    assert first.kind == "FULFILLED"
    assert "trailing" not in pulled   # produced lazily, source not drained


def test_run_collects_iter_verdicts_with_coverage():
    # run() is the batch face of the same generator; it must preserve the
    # coverage stats the streaming face cannot carry in its verdict yield.
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_declared", expects="tool_called",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_declared", "search", ts=0.0),
        ZeusEvent("log", None, ts=10.0),
        ZeusEvent("tool_called", "search", ts=20.0),
    ]

    result = run(stream, spec)

    assert [v.kind for v in result.verdicts] == ["FULFILLED"]
    assert result.total_events == 3
    assert result.unmatched_events == 1


def test_tick_expires_a_silent_obligation():
    # premortem #1 (the silence problem): an obligation opens, the agent goes
    # quiet, and only a synthetic tick carries the timestamp that crosses the
    # deadline. The BOLT must fire on the tick, mid-silence — not at end-of-stream.
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_called", "search", ts=0.0),
        ZeusEvent("tick", None, ts=5001.0),   # silence; clock crosses the window
    ]

    result = run(stream, spec)

    assert _kinds(result) == ["BOLTED"]
    v = result.verdicts[0]
    assert v.identifier == "search"
    assert v.closed_at == 5001.0
    assert v.detail["reason"] == "liveness_window_expired"


def test_tick_before_deadline_does_not_bolt():
    # a tick that has not yet crossed the window leaves the obligation open;
    # the only BOLT is the end-of-stream one (proves the tick gates on ts).
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_called", "search", ts=0.0),
        ZeusEvent("tick", None, ts=4999.0),   # still inside the window
    ]

    result = run(stream, spec)

    assert _kinds(result) == ["BOLTED"]
    assert result.verdicts[0].detail["reason"] == "session_ended_unresolved"


def test_ticks_do_not_count_against_coverage():
    # premortem #5/#6: ticks are pure time carriers. They must not inflate the
    # coverage denominator, or a quiet live session reads as "all blind".
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_called", "search", ts=0.0),   # triggers -> matched
        ZeusEvent("tick", None, ts=1000.0),           # carrier, not an event
        ZeusEvent("tick", None, ts=2000.0),
        ZeusEvent("tool_result", "search", ts=3000.0),  # fulfills -> matched
    ]

    result = run(stream, spec)

    assert result.total_events == 2       # the two real events, not the ticks
    assert result.unmatched_events == 0


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


def test_concurrent_same_tool_results_close_matching_obligation():
    # premortem #2: two concurrent calls to the same tool must not cross-close.
    # Only the result carrying the same correlation id fulfills its obligation;
    # the other call is still unresolved and BOLTs at end of stream.
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_called", "search", correlation="a", ts=0.0),
        ZeusEvent("tool_called", "search", correlation="b", ts=10.0),
        # result for call "a" arrives first; it must NOT close call "b"
        ZeusEvent("tool_result", "search", correlation="a", ts=20.0),
    ]

    result = run(stream, spec)

    fulfilled = [v for v in result.verdicts if v.kind == "FULFILLED"]
    bolted = [v for v in result.verdicts if v.kind == "BOLTED"]
    assert len(fulfilled) == 1
    assert fulfilled[0].identifier == "search"
    assert fulfilled[0].detail.get("correlation") == "a"
    assert len(bolted) == 1
    assert bolted[0].identifier == "search"
    assert bolted[0].detail.get("correlation") == "b"


def test_correlation_absent_falls_back_to_identifier_match():
    # Obligations/events created without a correlation id still match by
    # identifier alone, preserving M0/M1 replay compatibility.
    spec = Spec(triggers=[
        TriggerRule(on_primitive="tool_called", expects="tool_result",
                    window_ms=5000, spec_line=1),
    ])
    stream = [
        ZeusEvent("tool_called", "search", ts=0.0),
        ZeusEvent("tool_called", "search", ts=10.0),
        ZeusEvent("tool_result", "search", ts=20.0),
    ]

    result = run(stream, spec)

    fulfilled = [v for v in result.verdicts if v.kind == "FULFILLED"]
    bolted = [v for v in result.verdicts if v.kind == "BOLTED"]
    assert len(fulfilled) == 1
    assert len(bolted) == 1
